// 【阅读导航】模型渲染适配层：属性匹配、DMU 插件加载、共享模型缓存及独立实例变换均在此处。
#include "veylaxis/streaming/ModelRegistry.hpp"
#include "veylaxis/RuntimeEnvironment.hpp"
#include "veylaxis/vgds/Grid.hpp"
#include <osg/MatrixTransform>
#include <osg/ValueObject>
#include <osgDB/ReadFile>
#include <osgDB/Options>
#include <osgDB/Registry>
#include <cctype>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <set>
#include <charconv>
#include <cmath>
#include <algorithm>
#include <stdexcept>

namespace veylaxis::streaming
{
namespace
{
using namespace veylaxis::vgds;
using J = nlohmann::json;
namespace fs = std::filesystem;
using Token = std::variant<std::int64_t, std::string>;

std::string extension(const fs::path& p)
{
    auto e = p.extension().string();
    std::transform(e.begin(), e.end(), e.begin(),
                   [](unsigned char c)
                   {
                       return static_cast<char>(std::tolower(c));
                   });
    return e;
}

bool supported(const fs::path& p)
{
    auto e = extension(p);
    return e == ".osgb" || e == ".dmu3d" || e == ".dmuship";
}

// DMU 首次使用时显式加载项目插件，并注册扩展名别名；不在 VGDS Provider 里绑定模型。
bool dmuPlugin()
{
    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);
    static bool loaded = false;
    if (loaded)
        return true;
    wchar_t executable[32768]{};
    GetModuleFileNameW(nullptr, executable, 32768);
    std::vector<fs::path> paths{fs::path(executable).parent_path() / "osgPlugins-3.6.5/osgdb_dmu.dll",
                                fs::u8path(VEYLAXIS_DMU_PLUGIN_DIR) / "osgdb_dmu.dll"};
    if (auto externalPlugin = std::getenv("DMU_PLUGIN_PATH"))
        paths.push_back(fs::u8path(externalPlugin));
    auto* registry = osgDB::Registry::instance();
    for (const auto& path : paths)
        if (fs::is_regular_file(path))
        {
            auto result = registry->loadLibrary(path.u8string());
            if (result != osgDB::Registry::NOT_LOADED)
            {
                registry->addFileExtensionAlias("dmu3d", "dmu");
                registry->addFileExtensionAlias("dmuship", "dmu");
                loaded = true;
                break;
            }
        }
    return loaded;
}

Token token(const std::string& text)
{
    std::int64_t number = 0;
    auto parsed = std::from_chars(text.data(), text.data() + text.size(), number);
    if (!text.empty() && parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size())
        return number;
    return text;
}

Token token(const J& j)
{
    if (j.is_string())
        return token(j.get<std::string>());
    if (j.is_number_integer())
    {
        if (j.is_number_unsigned() && j.get<std::uint64_t>() > INT64_MAX)
            throw std::invalid_argument("Selector integer overflow");
        return j.get<std::int64_t>();
    }
    throw std::invalid_argument("Selector value must be an integer or string");
}

bool matches(const Property& property, const Token& expected)
{
    if (auto value = std::get_if<std::int64_t>(&property))
        return expected == Token(*value);
    if (auto value = std::get_if<std::string>(&property))
        return expected == token(*value);
    if (auto values = std::get_if<std::vector<std::string>>(&property))
        return std::any_of(values->begin(), values->end(),
                           [&](const auto& value)
                           {
                               return expected == token(value);
                           });
    if (auto value = std::get_if<double>(&property))
    {
        if (auto number = std::get_if<std::int64_t>(&expected))
            return std::isfinite(*value) && std::abs(*value) <= 9007199254740991.0 &&
                   *value == static_cast<double>(*number) &&
                   std::abs(static_cast<long double>(*number)) <= 9007199254740991.0L;
    }
    return false; // missing, null and boolean never imply a category
}

double number(const J& j)
{
    if (!j.is_number())
        throw std::invalid_argument("Transform component must be numeric");
    auto n = j.get<double>();
    if (!std::isfinite(n))
        throw std::invalid_argument("Transform component must be finite");
    return n;
}

std::array<double, 3> triple(const J& j)
{
    if (!j.is_array() || j.size() != 3)
        throw std::invalid_argument("Transform needs three components");
    return {number(j[0]), number(j[1]), number(j[2])};
}
}

struct ModelRegistry::Impl
{
    struct Rule
    {
        std::string id, objectClass, attribute, path;
        std::optional<Token> value;
        std::array<double, 3> scale{1, 1, 1}, rotation{0, 0, 0};
        double height{};
    };

    struct Asset
    {
        osg::ref_ptr<osg::Node> node;
        fs::file_time_type modified;
        std::uintmax_t bytes{};
        std::uint64_t touched{};
    };

    std::string configuration;
    WarningSink warning;
    mutable std::mutex mutex;
    std::vector<Rule> rules;
    std::map<std::string, Asset> assets;
    std::set<std::string> warned;
    std::size_t capacity{64};
    std::uint64_t clock{};
    ModelRegistryStats counters;

    void warn(const std::string& key, const std::string& message)
    {
        if (!warned.insert(key).second)
            return;
        ++counters.warnings;
        try
        {
            warning("[navigation models] " + message);
        }
        catch (...)
        {
        }
    }

    // 有属性条件的规则优先于同类无条件规则；缺失属性不猜测默认类别。
    const Rule* match(const Feature& f) const
    {
        const Rule* fallback = nullptr;
        for (const auto& r : rules)
        {
            if (r.objectClass != f.objectType)
                continue;
            if (r.attribute.empty())
            {
                if (!fallback)
                    fallback = &r;
                continue;
            }
            auto property = f.properties.find(r.attribute);
            if (property != f.properties.end() && matches(property->second, *r.value))
                return &r;
        }
        return fallback;
    }

    // 缓存键是规范化路径；文件修改后重新读取，活动实例继续持有其原有资源。
    osg::ref_ptr<osg::Node> load(const Rule& rule)
    {
        if (rule.path.empty())
        {
            warn("unconfigured:" + rule.id, "No modelPath configured for " + rule.id);
            return {};
        }
        try
        {
            auto path = fs::u8path(rule.path);
            if (!fs::is_regular_file(path))
            {
                warn("missing:" + rule.path, "Model file not found: " + rule.path);
                return {};
            }
            auto modified = fs::last_write_time(path);
            auto bytes = fs::file_size(path);
            auto found = assets.find(rule.path);
            if (found != assets.end() && found->second.modified == modified && found->second.bytes == bytes)
            {
                found->second.touched = ++clock;
                ++counters.cacheHits;
                return found->second.node;
            }
            if (extension(path) != ".osgb" && !dmuPlugin())
            {
                warn("dmu-plugin", "Cannot load project-side osgdb_dmu.dll");
                return {};
            }
            osg::ref_ptr<osgDB::Options> options = new osgDB::Options(
                std::getenv("DMU_READER_OPTIONS") ? std::getenv("DMU_READER_OPTIONS") : "");
            options->setObjectCacheHint(osgDB::Options::CACHE_NONE);
            ++counters.diskLoads;
            auto node = osgDB::readRefNodeFile(rule.path, options);
            // Initialize immutable shared resources before publishing to the scene thread.
            if (node)
            {
                node->setThreadSafeRefUnref(true);
                node->getBound();
            }
            assets[rule.path] = {node, modified, bytes, ++clock};
            while (assets.size() > capacity)
            {
                auto oldest = std::min_element(assets.begin(), assets.end(),
                                               [](const auto& a, const auto& b)
                                               {
                                                   return a.second.touched < b.second.touched;
                                               });
                assets.erase(oldest);
                ++counters.evictions;
            }
            if (!node)
                warn("unreadable:" + rule.path, "Cannot load model: " + rule.path);
            return node;
        }
        catch (const std::exception& e)
        {
            warn("load:" + rule.path, "Model load skipped: " + rule.path + " (" + e.what() + ")");
            return {};
        }
    }
};

ModelRegistry::ModelRegistry(std::string path, WarningSink sink) : impl_(std::make_unique<Impl>())
{
    veylaxis::loadRuntimeEnvironment();
    impl_->configuration = std::move(path);
    impl_->warning = sink ? std::move(sink)
                          : WarningSink(
                                [](const std::string& message)
                                {
                                    std::cerr << message << '\n';
                                });
    reload();
}

ModelRegistry::~ModelRegistry() = default;

osg::ref_ptr<osg::Node> ModelRegistry::loadModel(const std::string& path)
{
    auto& i = *impl_;
    std::lock_guard<std::mutex> lock(i.mutex);
    try
    {
        if (path.empty())
            return {};
        auto p = fs::u8path(path);
        if (!supported(p))
            throw std::invalid_argument("Unsupported model extension");
        if (p.is_relative())
            p = fs::absolute(fs::u8path(i.configuration)).parent_path() / p;
        Impl::Rule rule;
        rule.id = "vessel";
        rule.path = fs::weakly_canonical(p).u8string();
        return i.load(rule);
    }
    catch (const std::exception& e)
    {
        i.warn("model-path", e.what());
        return {};
    }
}

// 先完整解析新配置，成功后才替换规则；配置损坏时保留上次有效配置。
bool ModelRegistry::reload()
{
    auto& i = *impl_;
    std::lock_guard<std::mutex> lock(i.mutex);
    try
    {
        fs::path path = fs::absolute(fs::u8path(i.configuration));
        if (fs::file_size(path) > 1024 * 1024)
            throw std::invalid_argument("Mapping file exceeds 1MiB");
        std::ifstream input(path);
        J config;
        input >> config;
        if (config.at("version") != 1)
            throw std::invalid_argument("Unsupported navigation mapping version");
        const auto& mappings = config.at("mappings");
        if (!mappings.is_array() || mappings.size() > 256)
            throw std::invalid_argument("Expected at most 256 mappings");
        auto capacity = config.value("maxCachedModels", 64u);
        if (capacity < 1 || capacity > 256)
            throw std::invalid_argument("Invalid model cache capacity");
        std::vector<Impl::Rule> rules;
        std::set<std::string> ids;
        for (const auto& value : mappings)
        {
            if (!value.value("enabled", true))
                continue;
            Impl::Rule r;
            r.id = value.at("id").get<std::string>();
            r.objectClass = value.at("objectClass").get<std::string>();
            r.path = value.at("modelPath").get<std::string>();
            if (r.id.empty() || r.objectClass.empty() || !ids.insert(r.id).second)
                throw std::invalid_argument("Empty or duplicate mapping identity");
            if (value.contains("attribute"))
            {
                r.attribute = value.at("attribute").get<std::string>();
                if (r.attribute.empty())
                    throw std::invalid_argument("Empty selector attribute");
                r.value = token(value.at("value"));
            }
            else if (value.contains("value"))
                throw std::invalid_argument("Selector value without attribute");
            if (value.contains("scale"))
            {
                if (value["scale"].is_array())
                    r.scale = triple(value["scale"]);
                else
                    r.scale.fill(number(value["scale"]));
            }
            for (double n : r.scale)
                if (n <= 0)
                    throw std::invalid_argument("Scale must be positive");
            if (value.contains("rotation"))
                r.rotation = triple(value["rotation"]);
            if (value.contains("heightOffset"))
                r.height = number(value["heightOffset"]);
            if (!r.path.empty())
            {
                auto model = fs::u8path(r.path);
                if (!supported(model))
                    throw std::invalid_argument("Model must be .osgb, .dmu3D or .dmuShip");
                if (model.is_relative())
                    model = path.parent_path() / model;
                r.path = fs::weakly_canonical(model).u8string();
            }
            rules.push_back(std::move(r));
        }
        i.rules = std::move(rules);
        i.capacity = capacity;
        i.assets.clear();
        i.warned.clear();
        return true;
    }
    catch (const std::exception& e)
    {
        i.warn("configuration",
               "Cannot read " + i.configuration + ": " + e.what() + "; keeping previous valid mappings");
        return false;
    }
}

std::optional<std::string> ModelRegistry::matchingRule(const Feature& f) const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (auto rule = impl_->match(f))
        return rule->id;
    return {};
}

// 位置按 Tile 原点转为局部米制坐标；重复来源 ID 和不属于本 Tile 的点不再实例化。
osg::ref_ptr<osg::Group> ModelRegistry::instantiate(const TilePackage& package)
{
    auto& i = *impl_;
    std::lock_guard<std::mutex> lock(i.mutex);
    osg::ref_ptr<osg::Group> group = new osg::Group;
    group->setName("navigation-model-instances");
    std::set<std::string> seen;
    for (const auto& f : package.features)
    {
        if (f.layer != "navigation_aids")
            continue;
        auto point = std::get_if<Point>(&f.geometry);
        if (!point)
        {
            ++i.counters.skipped;
            i.warn("geometry:" + f.objectType,
                   "Non-point " + f.objectType + " has no point-model placement; no centroid is inferred");
            continue;
        }
        auto owner = locate(package.metadata.grid, point->position);
        if (f.sourceObjectId.empty() || !std::holds_alternative<TileID>(owner) ||
            !(std::get<TileID>(owner) == package.metadata.tile))
        {
            ++i.counters.skipped;
            i.warn("ownership", "Ignoring navigation point without identity or correct Tile ownership");
            continue;
        }
        if (!seen.insert(f.sourceObjectId).second)
        {
            ++i.counters.duplicates;
            continue;
        }
        auto rule = i.match(f);
        if (!rule)
        {
            ++i.counters.skipped;
            i.warn("unmatched:" + f.objectType, "No matching rule for " + f.objectType +
                                                    " (missing/unknown categories are not defaulted)");
            continue;
        }
        auto model = i.load(*rule);
        if (!model)
        {
            ++i.counters.skipped;
            continue;
        }
        const double radians = std::acos(-1.0) / 180;
        auto matrix =
            osg::Matrixd::scale(rule->scale[0], rule->scale[1], rule->scale[2]) *
            osg::Matrixd::rotate(rule->rotation[0] * radians, osg::Vec3d(1, 0, 0)) *
            osg::Matrixd::rotate(rule->rotation[1] * radians, osg::Vec3d(0, 1, 0)) *
            osg::Matrixd::rotate(rule->rotation[2] * radians, osg::Vec3d(0, 0, 1)) *
            osg::Matrixd::translate(point->position.x - package.metadata.tileOrigin.x,
                                    point->position.y - package.metadata.tileOrigin.y, rule->height);
        osg::ref_ptr<osg::MatrixTransform> placement = new osg::MatrixTransform;
        placement->setName(f.sourceObjectId);
        placement->setMatrix(matrix);
        placement->setUserValue("source_id", f.sourceObjectId);
        placement->setUserValue("data_version", f.dataVersion);
        placement->setUserValue("object_class", f.objectType);
        placement->setUserValue("mapping_id", rule->id);
        placement->setUserValue("model_path", rule->path);
        placement->addChild(model);
        group->addChild(placement);
        ++i.counters.instances;
    }
    return group;
}

ModelRegistryStats ModelRegistry::stats() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto stats = impl_->counters;
    stats.cachedModels = impl_->assets.size();
    return stats;
}
}
