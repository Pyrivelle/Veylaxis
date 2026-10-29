#include "veylaxis/streaming/ModelRegistry.hpp"
#include "veylaxis/streaming/TileManager.hpp"
#include "veylaxis/vgds/S57.hpp"
#include "veylaxis/vgds/Grid.hpp"
#include "veylaxis/vgds/Codec.hpp"
#include "veylaxis/vgds/Catalog.hpp"
#include <osg/MatrixTransform>
#include <osg/Geode>
#include <osg/ShapeDrawable>
#include <osg/observer_ptr>
#include <osgDB/WriteFile>
#include <nlohmann/json.hpp>
#include <ogrsf_frmts.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <chrono>
#include <set>
#include <cmath>

namespace
{
using namespace veylaxis::vgds;
using namespace veylaxis::streaming;
using J = nlohmann::json;
namespace fs = std::filesystem;
int checks;

void check(bool pass, const char* why)
{
    ++checks;
    if (!pass)
        throw std::runtime_error(why);
}

template <class T> T value(Result<T> r)
{
    if (auto e = std::get_if<TileError>(&r))
        throw std::runtime_error(e->message);
    return std::get<T>(std::move(r));
}

void save(const fs::path& p, const J& j)
{
    std::ofstream out(p);
    out << j.dump(2);
}

Feature point(std::string id, std::string type, XY p)
{
    Feature f;
    f.layer = "navigation_aids";
    f.objectType = std::move(type);
    f.sourceObjectId = std::move(id);
    f.fragmentId = f.sourceObjectId + ":fragment";
    f.dataVersion = "fixture-v1";
    f.geometry = Point{p};
    return f;
}

TilePackage package(const GridSpec& g, TileID id)
{
    MockProvider provider(g);
    return value(provider.request({"test", id, {"land"}}));
}

void settle(TileManager& manager, XY p)
{
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    for (;;)
    {
        check(!manager.tick(p), "Manager tick");
        auto s = manager.stats();
        if (s.active == 9 && s.loading == 0 && s.inFlight == 0 && s.completed == 0 && s.queued == 0)
            return;
        if (std::chrono::steady_clock::now() > deadline)
            throw std::runtime_error("Navigation manager timeout");
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}
}
int runNavigationModelTests()
try
{
    checks = 0;
    const fs::path source = VEYLAXIS_SOURCE_DIR;
    const auto temp =
        fs::temp_directory_path() /
        ("veylaxis-navigation-" +
         contentHash(std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))
             .substr(0, 16));
    fs::create_directories(temp);

    struct Cleanup
    {
        fs::path path;

        ~Cleanup()
        {
            std::error_code e;
            fs::remove_all(path, e);
        }
    } cleanup{temp};

    osg::ref_ptr<osg::Group> fixture = new osg::Group;
    fixture->setName("navigation-model-fixture");
    osg::ref_ptr<osg::Geode> mesh = new osg::Geode;
    mesh->addDrawable(new osg::ShapeDrawable(new osg::Box(osg::Vec3(0, 0, 1), 2, 2, 2)));
    fixture->addChild(mesh);
    check(osgDB::writeNodeFile(*fixture, (temp / "fixture.osgb").string()), "Write actual OSGB fixture");
    J config;
    {
        std::ifstream in(source / "config/navigation_models.json");
        in >> config;
    }
    check(config["mappings"].size() == 14, "All fourteen required mappings");
    for (auto& rule : config["mappings"])
        rule["modelPath"] = "fixture.osgb";
    save(temp / "models.json", config);
    std::vector<std::string> warnings;
    auto registry = std::make_shared<ModelRegistry>((temp / "models.json").string(),
                                                    [&](const auto& s)
                                                    {
                                                        warnings.push_back(s);
                                                    });
    GridSpec grid{"nav", "EPSG:32619", {503500, 4856500}, 1000};
    auto tile = package(grid, {grid.id, 0, 0});
    tile.features.clear();
    for (const auto& r : config["mappings"])
    {
        auto f = point(r["id"], r["objectClass"], {503600, 4856600});
        if (r.contains("attribute"))
            f.properties[r["attribute"].get<std::string>()] = r["value"].get<std::int64_t>();
        if (f.objectType == "BOYSPP")
            f.properties["CATSPM"] = std::vector<std::string>{"1", "2"};
        check(registry->matchingRule(f) == std::optional<std::string>(r["id"].get<std::string>()),
              "Synthetic category mapping");
        tile.features.push_back(f);
    }
    auto instances = registry->instantiate(tile);
    check(instances->getNumChildren() == 14, "Instantiate every supported type");
    auto stats = registry->stats();
    check(stats.diskLoads == 1 && stats.cacheHits == 13, "One disk read shared by fourteen objects");
    auto* a = instances->getChild(0)->asGroup();
    auto* b = instances->getChild(1)->asGroup();
    check(a != b && a->getChild(0) == b->getChild(0), "Unique transforms share model node");
    check(a->getChild(0)->getBound().valid(), "Loaded model contains actual 3D geometry");
    auto duplicate = tile.features.front();
    duplicate.fragmentId = "another-fragment";
    tile.features.push_back(duplicate);
    check(registry->instantiate(tile)->getNumChildren() == 14 && registry->stats().duplicates == 1,
          "Source identity dedup ignores fragment ID");
    tile.features.pop_back();
    auto nonpoint = tile;
    nonpoint.features = {point("line-facility", "MORFAC", {503600, 4856600})};
    nonpoint.features.front().geometry =
        LineString{{{503600, 4856600}, {503650, 4856650}}, {BoundaryKind::Geographic}};
    check(registry->instantiate(nonpoint)->getNumChildren() == 0,
          "Line facility is retained as data but never placed at inferred centroid");
    ModelRegistry absentConfiguration((temp / "absent.json").string(),
                                      [](const auto&)
                                      {
                                      });
    check(absentConfiguration.instantiate(tile)->getNumChildren() == 0,
          "Missing mapping configuration does not throw or fabricate models");
    auto f = tile.features.front();
    f.properties.clear();
    check(!registry->matchingRule(f), "Missing CATLAM has no default");
    f.properties["CATLAM"] = std::monostate{};
    check(!registry->matchingRule(f), "Null category has no default");
    f.properties["CATLAM"] = true;
    check(!registry->matchingRule(f), "Boolean cannot masquerade as integer category");
    for (auto n : {3, 4, 99})
    {
        f.properties["CATLAM"] = std::int64_t(n);
        check(!registry->matchingRule(f), "Unconfigured preferred channel category not port/starboard");
    }
    f.properties["CATLAM"] = std::string("1");
    check(registry->matchingRule(f) == std::optional<std::string>("port"), "Numeric-string selector");
    f.properties["CATLAM"] = std::vector<std::string>{"2"};
    check(registry->matchingRule(f) == std::optional<std::string>("starboard"), "List selector");
    // Projection grid ownership is half-open, including exact four-Tile corners.
    auto edge = point("edge", "BOYISD", {504500, 4857500});
    for (auto id : std::vector<TileID>{{grid.id, 0, 0}, {grid.id, 1, 0}, {grid.id, 0, 1}, {grid.id, 1, 1}})
    {
        auto p = package(grid, id);
        p.features = {edge};
        check(registry->instantiate(p)->getNumChildren() == (id.x == 1 && id.y == 1 ? 1u : 0u),
              "Boundary point belongs to one Tile");
    }
    auto one = tile;
    one.features = {tile.features.front()};
    auto transformed = config;
    transformed["mappings"][0]["scale"] = J::array({2, 3, 4});
    transformed["mappings"][0]["rotation"] = J::array({0, 0, 90});
    transformed["mappings"][0]["heightOffset"] = 5;
    save(temp / "models.json", transformed);
    check(registry->reload(), "Reload transformations without C++ change");
    auto placed = registry->instantiate(one);
    auto* transform = dynamic_cast<osg::MatrixTransform*>(placed->getChild(0));
    auto xyz = osg::Vec3d(1, 0, 0) * transform->getMatrix();
    check((xyz - osg::Vec3d(100, 102, 5)).length() < 1e-8, "Scale, degree rotation and tile-local placement");
    {
        std::ofstream bad(temp / "models.json");
        bad << "broken";
    }
    check(!registry->reload() && registry->instantiate(one)->getNumChildren() == 1,
          "Malformed config retains last valid mappings");
    auto missing = config;
    missing["mappings"][0]["modelPath"] = "missing.osgb";
    save(temp / "models.json", missing);
    check(registry->reload() && registry->instantiate(one)->getNumChildren() == 0,
          "Missing model skipped safely");
    fs::copy_file(temp / "fixture.osgb", temp / "missing.osgb");
    check(registry->instantiate(one)->getNumChildren() == 1, "Previously missing file recovers");
    missing["mappings"][0]["modelPath"] = "";
    save(temp / "models.json", missing);
    check(registry->reload() && registry->instantiate(one)->getNumChildren() == 0,
          "Empty path is optional and skipped");
    {
        std::ofstream bad(temp / "bad.osgb");
        bad << "invalid osgb";
    }
    missing["mappings"][0]["modelPath"] = "bad.osgb";
    save(temp / "models.json", missing);
    check(registry->reload(), "Valid configuration with corrupt asset");
    auto before = registry->stats().diskLoads;
    check(registry->instantiate(one)->getNumChildren() == 0 &&
              registry->instantiate(one)->getNumChildren() == 0 && registry->stats().diskLoads == before + 1,
          "Corrupt model negative cache prevents repeated disk reads");
    auto unicode = config;
    fs::copy_file(temp / "fixture.osgb", temp / fs::u8path(u8"航标模型.osgb"));
    unicode["mappings"][0]["modelPath"] = u8"航标模型.osgb";
    save(temp / "models.json", unicode);
    check(registry->reload() && registry->instantiate(one)->getNumChildren() == 1,
          "UTF-8 Chinese asset path");
    auto listConfig = config;
    listConfig["mappings"].push_back({{"id", "special-subtype"},
                                      {"objectClass", "BOYSPP"},
                                      {"attribute", "CATSPM"},
                                      {"value", 2},
                                      {"modelPath", "fixture.osgb"}});
    save(temp / "models.json", listConfig);
    check(registry->reload() &&
              registry->matchingRule(tile.features[8]) == std::optional<std::string>("special-subtype"),
          "Specific CATSPM list rule precedes class fallback");
    auto bounded = config;
    bounded["maxCachedModels"] = 2;
    for (int k = 0; k < 3; ++k)
    {
        auto name = "asset" + std::to_string(k) + ".osgb";
        fs::copy_file(temp / "fixture.osgb", temp / name);
        bounded["mappings"][k]["modelPath"] = name;
    }
    save(temp / "models.json", bounded);
    check(registry->reload(), "Bounded resource cache");
    auto alive = registry->instantiate(tile);
    check(registry->stats().cachedModels <= 2 && registry->stats().evictions > 0 &&
              alive->getNumChildren() == 14,
          "LRU eviction preserves active instances");
    save(temp / "models.json", config);
    check(registry->reload(), "Restore normal rules");
    // Simulated S-57 features traverse the actual Provider/JSON/TileLoader/Manager path.
    Dataset dataset;
    dataset.id = "navigation-fixture";
    dataset.version = "v1";
    dataset.grid = grid;
    dataset.supportedLayers = {"land", "coastline", "navigation_aids"};
    dataset.features = tile.features;
    ServiceConfig serviceConfig;
    serviceConfig.cacheDirectory = (temp / "cache").string();
    auto provider = std::make_shared<LocalService>(dataset, serviceConfig);
    auto client = std::make_shared<TileClient>(
        [provider](const std::string& s)
        {
            return provider->requestJson(s);
        });
    auto roundtrip = value(client->request({"nav", {grid.id, 0, 0}, {"navigation_aids"}}));
    check(roundtrip.features.size() == 14, "All simulated source types survive Provider JSON");
    bool special = false;
    for (const auto& feature : roundtrip.features)
        if (feature.objectType == "BOYSPP")
            special = std::get<std::vector<std::string>>(feature.properties.at("CATSPM")) ==
                      std::vector<std::string>{"1", "2"};
    check(special, "Raw CATSPM retained");
    osg::ref_ptr<osg::Group> root = new osg::Group;
    osg::observer_ptr<osg::Node> weak;
    {
        ManagerConfig options;
        options.layers = {"navigation_aids"};
        TileManager manager(grid, root, makeLoader(client, {}, registry), options);
        settle(manager, {503600, 4856600});
        for (unsigned k = 0; k < root->getNumChildren(); ++k)
        {
            auto* group = root->getChild(k)->asGroup()->getChild(0)->asGroup()->getChild(1)->asGroup();
            if (group->getNumChildren())
                weak = group->getChild(0);
        }
        check(weak.valid(), "Navigation attached through actual TileLoader");
        auto requests = manager.stats().requests;
        for (int k = 0; k < 20; ++k)
            manager.tick({503600, 4856600});
        check(manager.stats().requests == requests, "Stationary view does not duplicate requests");
        settle(manager, {513600, 4856600});
        check(!weak.valid(), "Navigation placement freed on Tile unload");
        manager.clear();
        check(root->getNumChildren() == 0, "Clear detaches navigation Tiles");
    }
    // Missing model still permits all nine Tiles to become active.
    save(temp / "models.json", missing);
    check(registry->reload(), "Missing/corrupt model setup");
    {
        ManagerConfig options;
        options.layers = {"navigation_aids"};
        TileManager manager(grid, root, makeLoader(client, {}, registry), options);
        settle(manager, {503600, 4856600});
        check(manager.stats().failures == 0, "Bad asset cannot fail whole Tile");
    }
    save(temp / "models.json", config);
    check(registry->reload(), "Real ENC rules");
    S57Report report;
    auto actual = value(
        readS57((source / "S57_DATA/ENC_ROOT/US2EC04M/US2EC04M.000").string(), grid, &report, {false, true}));
    check(report.objectCounts["BOYLAT"] > 0, "Real ENC has navigation aids");
    const Feature* real = nullptr;
    for (const auto& feature : actual.features)
        if (feature.layer == "navigation_aids" && std::holds_alternative<Point>(feature.geometry) &&
            registry->matchingRule(feature))
        {
            real = &feature;
            break;
        }
    check(real != nullptr, "Real source has configured category");
    check(real->properties.count("OBJL") && real->properties.count("LNAM") &&
              real->properties.count("RCID") && real->properties.count("source_cell") &&
              real->dataVersion == actual.version,
          "Real source code, attributes and version retained");
    // Independently fetch the same raw S-57 object and transform lon/lat to the configured CRS.
    GDALAllRegister();
    auto* raw = static_cast<GDALDataset*>(
        GDALOpenEx((source / "S57_DATA/ENC_ROOT/US2EC04M/US2EC04M.000").string().c_str(),
                   GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr, nullptr));
    check(raw != nullptr, "Open raw ENC for independent coordinate check");
    std::unique_ptr<GDALDataset, decltype(&GDALClose)> rawOwner(raw, GDALClose);
    auto* layer = raw->GetLayerByName(real->objectType.c_str());
    bool compared = false;
    OGRSpatialReference src, dst;
    src.SetFromUserInput("EPSG:4326");
    dst.SetFromUserInput(grid.horizontalCrs.c_str());
    src.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    dst.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    auto* ct = OGRCreateCoordinateTransformation(&src, &dst);
    check(ct != nullptr, "Independent CRS conversion");
    while (auto* record = layer->GetNextFeature())
    {
        bool same = std::string(record->GetFieldAsString("LNAM")) ==
                    std::get<std::string>(real->properties.at("LNAM"));
        if (same)
        {
            auto* p = record->GetGeometryRef()->toPoint();
            double x = p->getX(), y = p->getY();
            check(ct->Transform(1, &x, &y), "Project raw navigation point");
            auto expected = std::get<Point>(real->geometry).position;
            check(std::hypot(x - expected.x, y - expected.y) < 1e-5, "Provider uses configured projection");
            compared = true;
        }
        OGRFeature::DestroyFeature(record);
        if (compared)
            break;
    }
    OCTDestroyCoordinateTransformation(ct);
    check(compared, "Compared actual source LNAM");
    auto realTile = value(locate(grid, std::get<Point>(real->geometry).position));
    LocalService realProvider(actual, ServiceConfig{(temp / "real-cache").string()});
    auto response = value(realProvider.request({"actual", realTile, {"navigation_aids"}}));
    auto realInstances = registry->instantiate(response);
    bool found = false;
    for (unsigned k = 0; k < realInstances->getNumChildren(); ++k)
        found |= realInstances->getChild(k)->getName() == real->sourceObjectId;
    check(found, "Real ENC object maps to OSGB via TilePackage");
    auto empty = value(realProvider.request({"empty", {grid.id, 10000, 10000}, {"navigation_aids"}}));
    check(empty.features.empty() && registry->instantiate(empty)->getNumChildren() == 0,
          "No source objects returns empty without failure");
    // Existing Catalog selection/cache remains the source of truth; no renderer source lookup.
    CatalogConfig catalogConfig;
    catalogConfig.root = (source / "S57_DATA/ENC_ROOT/US2EC04M").string();
    catalogConfig.indexFile = (temp / "catalog.json").string();
    catalogConfig.maritime = true;
    auto catalog = std::make_shared<EncCatalog>(grid, catalogConfig);
    CatalogProvider catalogProvider(catalog, ServiceConfig{(temp / "catalog-cache").string()});
    auto catalogResult = value(catalogProvider.request({"catalog-nav", realTile, {"navigation_aids"}}));
    std::set<std::string> sourceIds;
    for (const auto& feature : catalogResult.features)
        check(sourceIds.insert(feature.sourceObjectId).second, "Catalog has unique source identities");
    check(registry->instantiate(catalogResult)->getNumChildren() > 0,
          "Catalog-selected navigation points map to OSGB");
    auto hits = catalogProvider.stats().cacheHits;
    value(catalogProvider.request({"catalog-nav-again", realTile, {"navigation_aids"}}));
    check(catalogProvider.stats().cacheHits == hits + 1, "Navigation package uses existing Catalog cache");
    fs::create_directories(source / "output/vgds");
    save(source / "output/vgds/navigation-model-validation.json",
         J{{"checks", checks},
           {"synthetic_mapping_types", 14},
           {"real_cell", report.cell},
           {"real_object_class", real->objectType},
           {"real_source_id", real->sourceObjectId},
           {"real_classes", report.objectCounts},
           {"catalog_instances", registry->instantiate(catalogResult)->getNumChildren()},
           {"projection_tolerance_metres", 0.00001},
           {"dynamic_unload_verified", true},
           {"user_models_tested", false}});
    std::cout << "Navigation models: " << checks << " checks passed; 14 simulated categories, real "
              << real->objectType
              << ", disk sharing, projection, dedup, missing assets and unload verified\n";
    return 0;
}
catch (const std::exception& e)
{
    std::cerr << "Navigation model test failed: " << e.what() << '\n';
    return 1;
}

int runDmuModelTests()
try
{
    checks = 0;
    fs::path root = VEYLAXIS_SOURCE_DIR;
    auto path = root / "config/navigation_models.local.json";
    if (!fs::exists(path))
        path = root / "config/navigation_models.json";
    J config;
    {
        std::ifstream in(path);
        in >> config;
    }
    auto shipPath = config.value("vesselModelPath", std::string{});
    if (shipPath.empty() || !fs::is_regular_file(fs::u8path(shipPath)))
    {
        std::cout << "DMU asset unavailable; external-asset integration test skipped\n";
        return 77;
    }
    ModelRegistry registry(path.u8string());
    auto ship = registry.loadModel(shipPath);
    check(ship.valid() && ship->getBound().valid(), "Real dmuShip plugin read and valid geometry bounds");
    check(registry.loadModel(shipPath) == ship && registry.stats().diskLoads == 1,
          "Real ship resource reused");
    unsigned loaded = 0;
    for (const auto& rule : config["mappings"])
    {
        auto model = rule.at("modelPath").get<std::string>();
        if (model.empty())
            continue;
        auto node = registry.loadModel(model);
        check(node.valid() && node->getBound().valid(), "Configured real navigation DMU model loaded");
        ++loaded;
    }
    check(loaded > 0, "At least one real navigation model tested");
    GridSpec grid{"dmu-test", "EPSG:32619", {503500, 4856500}, 1000};
    auto tile = package(grid, {grid.id, 0, 0});
    tile.features = {point("dmu-port", "BOYLAT", {503600, 4856600})};
    tile.features[0].properties["CATLAM"] = std::int64_t(1);
    auto instances = registry.instantiate(tile);
    check(instances->getNumChildren() == 1, "Real DMU buoy instantiated from S57 category");
    osg::observer_ptr<osg::Node> weak = instances->getChild(0);
    instances = nullptr;
    check(!weak.valid(), "Real DMU instance released independently of shared asset");
    auto bound = ship->getBound();
    save(root / "output/vgds/dmu-model-validation.json", J{{"ship", shipPath},
                                                           {"navigation_models_loaded", loaded},
                                                           {"ship_bound_radius_metres", bound.radius()},
                                                           {"checks", checks}});
    std::cout << "DMU models: ship and " << loaded << " navigation assets loaded; " << checks
              << " checks passed\n";
    return 0;
}
catch (const std::exception& e)
{
    std::cerr << "DMU test failed: " << e.what() << '\n';
    return 1;
}
