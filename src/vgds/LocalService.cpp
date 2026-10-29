// 【阅读导航】单数据集的本地 Provider：按图层/区块裁剪，返回语义数据，并可生成静态 OSGB 缓存。
#include "veylaxis/vgds/Service.hpp"
#include "veylaxis/vgds/Codec.hpp"
#include "veylaxis/vgds/Grid.hpp"
#include <nlohmann/json.hpp>
#define NOMINMAX
#include <windows.h>
#include <filesystem>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <mutex>
#include <set>
#include <stdexcept>
#include <type_traits>

namespace veylaxis::vgds
{
namespace
{
namespace fs = std::filesystem;
using J = nlohmann::json;

template <class T> T value(Result<T> r)
{
    if (auto e = std::get_if<TileError>(&r))
        throw std::runtime_error(e->message);
    return std::get<T>(std::move(r));
}

std::string read(const fs::path& p, std::uintmax_t limit = 16 * 1024 * 1024)
{
    if (fs::file_size(p) > limit)
        throw std::runtime_error("Cache entry too large");
    std::ifstream in(p, std::ios::binary);
    if (!in)
        throw std::runtime_error("Cannot open cache file");
    std::string out((std::istreambuf_iterator<char>(in)), {});
    if (in.bad())
        throw std::runtime_error("Cannot read cache file");
    return out;
}

void writeAtomic(const fs::path& path, const std::string& bytes)
{
    auto temporary = path;
    temporary += ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        out.close();
        if (!out)
            throw std::runtime_error("Cannot write cache file");
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Cannot commit cache file");
}

struct FileLock
{
    HANDLE handle;

    explicit FileLock(const fs::path& root)
        : handle(CreateFileW((root / "writer.lock").c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr))
    {
        if (handle == INVALID_HANDLE_VALUE)
            throw std::runtime_error("Cache in use by another local request process");
    }

    ~FileLock()
    {
        CloseHandle(handle);
    }
};

J geometryKey(const Geometry& geometry)
{
    auto ring = [](const Ring& r)
    {
        J j = J::array();
        for (std::size_t i = 0; i < r.points.size(); ++i)
            j.push_back({r.points[i].x, r.points[i].y, static_cast<int>(r.edges.at(i))});
        return j;
    };
    auto polygon = [&](const Polygon& p)
    {
        J j = J::array({ring(p.outer)});
        for (const auto& h : p.holes)
            j.push_back(ring(h));
        return j;
    };
    J out = {{"type", geometry.index()}};
    if (auto p = std::get_if<Polygon>(&geometry))
        out["value"] = polygon(*p);
    else if (auto m = std::get_if<MultiPolygon>(&geometry))
    {
        out["value"] = J::array();
        for (const auto& part : m->polygons)
            out["value"].push_back(polygon(part));
    }
    else if (auto l = std::get_if<LineString>(&geometry))
    {
        out["value"] = J::array();
        for (auto coordinate : l->points)
            out["value"].push_back({coordinate.x, coordinate.y});
        out["edges"] = J::array();
        for (auto e : l->edges)
            out["edges"].push_back(static_cast<int>(e));
    }
    else
    {
        auto position = std::get<Point>(geometry).position;
        out["value"] = {position.x, position.y};
    }
    return out;
}

std::string fingerprint(const Dataset& d)
{
    J j = {{"dataset", d.id},
           {"version", d.version},
           {"provenance", static_cast<int>(d.provenance)},
           {"grid",
            {d.grid.id, d.grid.horizontalCrs, d.grid.origin.x, d.grid.origin.y, d.grid.tileSizeMeters,
             d.grid.horizontalUnit}},
           {"features", J::array()},
           {"supported", d.supportedLayers},
           {"terrain",
            d.terrainSource ? d.terrainSource->algorithmVersion + ":" + d.terrainSource->verticalDatum : ""}};
    std::set<std::string> ids;
    for (const auto& f : d.features)
    {
        if (f.sourceObjectId.empty() || f.dataVersion.empty() || !ids.insert(f.sourceObjectId).second)
            throw std::invalid_argument("Source identities must be nonempty and unique");
        J attrs = J::object();
        for (const auto& a : f.properties)
        {
            J v;
            std::visit(
                [&](const auto& p)
                {
                    using T = std::decay_t<decltype(p)>;
                    if constexpr (!std::is_same_v<T, std::monostate>)
                        v = p;
                },
                a.second);
            attrs[a.first] = {{"kind", a.second.index()}, {"value", v}};
        }
        j["features"].push_back({{"id", f.sourceObjectId},
                                 {"version", f.dataVersion},
                                 {"layer", f.layer},
                                 {"object", f.objectType},
                                 {"provenance", static_cast<int>(f.provenance)},
                                 {"geometry", geometryKey(f.geometry)},
                                 {"properties", attrs}});
    }
    return contentHash(j.dump());
}

Bounds featureBounds(const Geometry& g)
{
    Bounds b{INFINITY, INFINITY, -INFINITY, -INFINITY};
    auto points = [&](const auto& p)
    {
        for (auto v : p)
        {
            if (!std::isfinite(v.x) || !std::isfinite(v.y))
                throw std::invalid_argument("Nonfinite source geometry");
            b.minX = std::min(b.minX, v.x);
            b.minY = std::min(b.minY, v.y);
            b.maxX = std::max(b.maxX, v.x);
            b.maxY = std::max(b.maxY, v.y);
        }
    };
    auto polygon = [&](const Polygon& p)
    {
        points(p.outer.points);
        for (const auto& h : p.holes)
            points(h.points);
    };
    if (auto p = std::get_if<Polygon>(&g))
        polygon(*p);
    else if (auto m = std::get_if<MultiPolygon>(&g))
        for (const auto& part : m->polygons)
            polygon(part);
    else if (auto l = std::get_if<LineString>(&g))
        points(l->points);
    else
        points(std::vector<XY>{std::get<Point>(g).position});
    return b;
}
}

struct LocalService::Impl
{
    Dataset data;
    ServiceConfig config;
    std::string signature;
    std::vector<Bounds> index;
    mutable std::mutex mutex;
    CacheStats stats;
    mutable std::mutex publishedMutex;
    mutable CacheStats published;
};

LocalService::LocalService(Dataset dataset, ServiceConfig config) : impl_(std::make_unique<Impl>())
{
    if (auto e = validate(dataset.grid))
        throw std::invalid_argument(e->message);
    if (dataset.id.empty() || dataset.version.empty() || config.cacheDirectory.empty())
        throw std::invalid_argument("Missing dataset/cache identity");
    if (config.osgbEnabled && (!config.builder || config.builderVersion.empty()))
        throw std::invalid_argument("OSGB enabled without builder");
    impl_->data = std::move(dataset);
    impl_->config = std::move(config);
    impl_->signature = fingerprint(impl_->data);
    if (impl_->config.sourceLoaded)
        impl_->config.sourceLoaded(impl_->data);
    for (const auto& f : impl_->data.features)
        impl_->index.push_back(featureBounds(f.geometry));
    fs::create_directories(impl_->config.cacheDirectory);
}

LocalService::~LocalService() = default;

const Dataset& LocalService::dataset() const
{
    return impl_->data;
}

CacheStats LocalService::stats() const
{
    std::unique_lock<std::mutex> guard(impl_->mutex, std::try_to_lock);
    std::lock_guard<std::mutex> published(impl_->publishedMutex);
    if (guard.owns_lock())
        impl_->published = impl_->stats;
    return impl_->published;
}

// 单源请求链路，可先读懂这里再读 Catalog 的多源覆盖优先级。
TileResult LocalService::request(const TileRequest& r)
{
    auto normalized = encodeRequestJson(r);
    if (auto e = std::get_if<TileError>(&normalized))
        return *e;
    auto request = value(decodeRequestJson(std::get<std::string>(normalized)));
    const auto& d = impl_->data;
    auto bounds = boundsOf(d.grid, r.tile);
    if (auto e = std::get_if<TileError>(&bounds))
        return *e;
    std::lock_guard<std::mutex> guard(impl_->mutex);
    ++impl_->stats.requests;
    try
    {
        fs::path root = fs::absolute(impl_->config.cacheDirectory);
        FileLock lock(root);
        request.requestId.clear();
        const auto key =
            contentHash(J{{"request", value(encodeRequestJson(request))},
                          {"source", impl_->signature},
                          {"algorithm", "rectangle-geos-v1"},
                          {"osgb", impl_->config.osgbEnabled},
                          {"builder", impl_->config.osgbEnabled ? impl_->config.builderVersion : ""}}
                            .dump());
        fs::path dir = root / key, package = dir / "tile.json", manifest = dir / "commit.json";
        fs::create_directories(dir);
        if (fs::exists(manifest))
            try
            {
                auto commit = J::parse(read(manifest));
                auto encoded = read(package);
                if (commit.at("key") != key || commit.at("sha256") != contentHash(encoded))
                    throw std::runtime_error("Invalid cache integrity");
                auto p = value(decodeJson(encoded));
                for (const auto& a : p.artifacts)
                {
                    if (fs::path(a.path) != dir / "tile.osgb" || fs::file_size(a.path) != a.bytes ||
                        fileHash(a.path) != a.contentHash)
                        throw std::runtime_error("Invalid OSGB cache integrity");
                }
                ++impl_->stats.hits;
                p.metadata.requestId = r.requestId;
                return p;
            }
            catch (const std::exception&)
            {
                ++impl_->stats.repairs;
            }
        TilePackage p;
        auto& m = p.metadata;
        m.requestId = "";
        m.datasetId = d.id;
        m.dataVersion = d.version;
        m.algorithmVersion = "rectangle-geos-v1";
        m.grid = d.grid;
        m.tile = r.tile;
        m.bounds = std::get<Bounds>(bounds);
        m.tileOrigin = {m.bounds.minX, m.bounds.minY};
        m.provenance = d.provenance;
        for (const auto& layer : request.layers)
        {
            m.layers[layer] = std::find(d.supportedLayers.begin(), d.supportedLayers.end(), layer) !=
                                      d.supportedLayers.end()
                                  ? LayerState::Empty
                                  : LayerState::Unsupported;
            if (layer == "terrain" && d.terrainSource)
            {
                auto terrain = sampleTerrain(d.grid, r.tile, r.terrainSpacingMeters, *d.terrainSource);
                if (auto e = std::get_if<TileError>(&terrain))
                    return *e;
                p.terrain = std::get<TerrainData>(std::move(terrain));
                m.layers[layer] =
                    std::find(p.terrain->valid.begin(), p.terrain->valid.end(), 1) != p.terrain->valid.end()
                        ? LayerState::Available
                        : LayerState::NoCoverage;
                m.algorithmVersion += "+" + d.terrainSource->algorithmVersion;
                continue;
            }
            for (std::size_t i = 0; i < d.features.size(); ++i)
            {
                if (d.features[i].layer != layer)
                    continue;
                auto b = impl_->index[i];
                if (b.maxX < m.bounds.minX || b.minX > m.bounds.maxX || b.maxY < m.bounds.minY ||
                    b.minY > m.bounds.maxY)
                    continue;
                auto pieces = clipFeature(d.features[i], m.bounds, r.tile);
                if (auto e = std::get_if<TileError>(&pieces))
                    return *e;
                for (auto& f : std::get<std::vector<Feature>>(pieces))
                {
                    p.features.push_back(std::move(f));
                    m.layers[layer] = LayerState::Available;
                }
            }
        }
        if (auto e = validate(p))
            return *e;
        if (impl_->config.osgbEnabled)
            p.artifacts.push_back(impl_->config.builder(p, (dir / "tile.osgb").string()));
        auto encoded = encodeJson(p);
        if (auto e = std::get_if<TileError>(&encoded))
            return *e;
        writeAtomic(package, std::get<std::string>(encoded));
        writeAtomic(manifest,
                    J{{"key", key}, {"sha256", contentHash(std::get<std::string>(encoded))}}.dump());
        ++impl_->stats.generations;
        p.metadata.requestId = r.requestId;
        return p;
    }
    catch (const std::exception& e)
    {
        return TileError{ErrorCode::IoFailure, e.what()};
    }
}

Result<std::string> LocalService::requestJson(const std::string& bytes)
{
    return requestProviderJson(*this, bytes);
}

Result<std::string> requestProviderJson(ITileProvider& provider, const std::string& bytes)
{
    auto r = decodeRequestJson(bytes);
    if (auto e = std::get_if<TileError>(&r))
        return *e;
    auto p = provider.request(std::get<TileRequest>(r));
    if (auto e = std::get_if<TileError>(&p))
        return *e;
    return encodeJson(std::get<TilePackage>(p));
}
}
