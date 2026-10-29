#include "veylaxis/vgds/Catalog.hpp"
#include "veylaxis/vgds/Codec.hpp"
#include "veylaxis/vgds/Grid.hpp"
#include "veylaxis/streaming/Motion.hpp"
#include <filesystem>
#include <chrono>
#include <thread>
#include <future>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#ifdef VEYLAXIS_HAS_OSGB
#include "veylaxis/vgds/OsgBuilder.hpp"
#include "veylaxis/streaming/TileManager.hpp"
#include <osg/observer_ptr>
#endif
namespace
{
using namespace veylaxis::vgds;
using Clock = std::chrono::steady_clock;
int checks;

void check(bool v, const char* m)
{
    ++checks;
    if (!v)
        throw std::runtime_error(m);
}

template <class T> T value(Result<T> r)
{
    if (auto e = std::get_if<TileError>(&r))
        throw std::runtime_error(e->message);
    return std::get<T>(std::move(r));
}
}
int runAcceptanceTests()
try
{
    checks = 0;
    namespace fs = std::filesystem;
    using J = nlohmann::json;
    const auto root = fs::path(VEYLAXIS_SOURCE_DIR);
    const auto temp =
        fs::temp_directory_path() /
        ("veylaxis-s7-" + contentHash(std::to_string(Clock::now().time_since_epoch().count())).substr(0, 12));
    fs::create_directories(temp);
    GridSpec grid{"utm19-demo-v1", "EPSG:32619", {503500, 4856500}, 1000};
    CatalogConfig config;
    config.root = (root / "S57_DATA/ENC_ROOT").string();
    config.indexFile = (temp / "index.json").string();
    config.maxResidentCells = 1;
    config.maxCacheEntries = 12;
    config.bathymetry = true;
    config.maritime = true;
    auto catalog = std::make_shared<EncCatalog>(grid, config);
    ServiceConfig options;
    options.cacheDirectory = (temp / "cache").string();
    auto provider = std::make_shared<CatalogProvider>(catalog, options);
    TileRequest request{"cold", {grid.id, 0, 0}, {"terrain"}};
    std::vector<double> latency;
    for (int i = 0; i < 4; ++i)
    {
        request.requestId = "bench:" + std::to_string(i);
        auto start = Clock::now();
        auto p = value(provider->request(request));
        latency.push_back(std::chrono::duration<double, std::milli>(Clock::now() - start).count());
        check(p.terrain.has_value(), "Actual catalog seabed response");
    }
    check(provider->stats().cacheHits == 3, "Warm requests use persistent tile cache");
    // Finest chart wins at an actual overlapping detailed-cell test point.
    std::optional<XY> detailed;
    for (const auto& e : catalog->entries())
        if (e.compilationScale < 100000)
        {
            XY p{(e.bounds.minX + e.bounds.maxX) / 2, (e.bounds.minY + e.bounds.maxY) / 2};
            auto owner = catalog->owner(p);
            if (owner && catalog->entries()[*owner].compilationScale <= e.compilationScale)
            {
                detailed = p;
                break;
            }
        }
    check(detailed.has_value(), "Detailed ENC overlap test location");
    auto fineTile = value(locate(grid, *detailed));
    auto fine = value(provider->request({"detail", fineTile, {"land", "navigation_aids"}}));
    check(!validate(fine), "Detailed ENC auto-selection produces valid data");
    check(provider->stats().cellEvictions > 0 && provider->stats().residentCells <= 1,
          "Source LRU releases previous cell");
    provider->cancel("pre-cancelled");
    auto cancelled = provider->request({"pre-cancelled", request.tile, {"terrain"}});
    check(std::holds_alternative<TileError>(cancelled) &&
              std::get<TileError>(cancelled).code == ErrorCode::Cancelled,
          "Pre-start cancellation");
    auto future = std::async(std::launch::async,
                             [&]
                             {
                                 return provider->request({"active-cancel", request.tile, {"terrain"}, 5});
                             });
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    provider->cancel("active-cancel");
    auto interrupted = future.get();
    check(std::holds_alternative<TileError>(interrupted) &&
              std::get<TileError>(interrupted).code == ErrorCode::Cancelled,
          "In-flight terrain computation is cancellable");
    {
        bool stopped = false;
        RequestScope scope(
            []
            {
                return true;
            });
        try
        {
            requestCheckpoint();
        }
        catch (const RequestCancelled&)
        {
            stopped = true;
        }
        check(stopped, "Cooperative checkpoint");
    }
    auto shortConfig = config;
    shortConfig.requestTimeoutMilliseconds = 1;
    auto deadlineCatalog = std::make_shared<EncCatalog>(grid, shortConfig);
    auto deadlineOptions = options;
    deadlineOptions.cacheDirectory = (temp / "deadline").string();
    CatalogProvider deadlineProvider(deadlineCatalog, deadlineOptions);
    auto expired = deadlineProvider.request({"deadline", request.tile, {"terrain"}, 5});
    check(std::holds_alternative<TileError>(expired) &&
              std::get<TileError>(expired).code == ErrorCode::Cancelled,
          "Configured request deadline enforced");
    std::promise<void> entered, release;
    auto released = release.get_future().share();
    auto blockedOptions = options;
    blockedOptions.cacheDirectory = (temp / "blocked").string();
    blockedOptions.sourceLoaded = [&](const Dataset&)
    {
        entered.set_value();
        released.wait();
    };
    auto blocked = std::make_shared<CatalogProvider>(catalog, blockedOptions);
    auto work = std::async(std::launch::async,
                           [&]
                           {
                               return blocked->request({"blocked", request.tile, {"land"}});
                           });
    auto reached = entered.get_future().wait_for(std::chrono::seconds(25));
    if (reached != std::future_status::ready)
    {
        release.set_value();
        throw std::runtime_error("Stats blocking test did not reach source callback");
    }
    auto statsStart = Clock::now();
    blocked->stats();
    auto statsMs = std::chrono::duration<double, std::milli>(Clock::now() - statsStart).count();
    release.set_value();
    value(work.get());
    check(statsMs < 200, "Progress reporting never waits for source generation lock");
    // Rapid positions are simulated continuously rather than jumping Tile IDs.
    veylaxis::streaming::VesselMotion motion;
    motion.start = motion.position = {499241.046445, 4861249.777884};
    motion.automatic = true;
    motion.speed = 250;
    motion.tileSize = grid.tileSizeMeters;
    auto original = motion.position;
    motion.advance(2);
    check(motion.position.x == original.x + 500, "Metre/second integration");
    motion.paused = true;
    motion.advance(10);
    check(motion.position.x == original.x + 500, "Motion pause");
    motion.paused = false;
    std::uint64_t requests = 0, unloaded = 0, cancellations = 0, peakMesh = 0;
#ifdef VEYLAXIS_HAS_OSGB
    using namespace veylaxis::streaming;
    IslandTerrainConfig terrain;
    terrain.enabled = true;
    auto field = std::make_shared<IslandTerrain>(terrain);
    ServiceConfig rendered = options;
    rendered.cacheDirectory = (temp / "rendered").string();
    rendered.osgbEnabled = true;
    rendered.builderVersion = field->version();
    rendered.builder = proceduralBuilder(field);
    rendered.sourceLoaded = [field](const Dataset& d)
    {
        field->addDataset(d);
    };
    auto service = std::make_shared<CatalogProvider>(catalog, rendered);
    auto client = std::make_shared<TileClient>(
        [service](const std::string& bytes)
        {
            return requestProviderJson(*service, bytes);
        });
    osg::ref_ptr<osg::Group> scene = new osg::Group;
    std::vector<osg::observer_ptr<osg::Node>> observed;
    {
        ManagerConfig managerConfig;
        managerConfig.layers = {"land", "terrain", "navigation_aids", "fairway", "dredged_areas"};
        managerConfig.cancelRequest = [service](const std::string& id)
        {
            service->cancel(id);
        };
        TileManager manager(grid, scene, makeLoader(client, field), managerConfig);
        auto settle = [&](XY p)
        {
            auto deadline = Clock::now() + std::chrono::seconds(120);
            for (;;)
            {
                check(!manager.tick(p), "Motion requests valid grid position");
                auto s = manager.stats();
                check(s.active <= 9 && s.queued <= 9 && s.completed <= 9 && s.inFlight <= 1,
                      "Bounded scene and queues");
                peakMesh = std::max(peakMesh, s.activeMeshBytes);
                if (s.active == 9 && !s.loading && !s.queued && !s.inFlight)
                    break;
                if (Clock::now() > deadline)
                    throw std::runtime_error("S7 actual scene loading timeout");
                std::this_thread::sleep_for(std::chrono::milliseconds(3));
            }
        };
        settle(original);
        for (unsigned i = 0; i < scene->getNumChildren(); ++i)
            observed.emplace_back(scene->getChild(i));
        auto before = manager.stats().requests;
        for (int i = 0; i < 20; ++i)
            manager.tick(original);
        check(manager.stats().requests == before, "No duplicate requests for stable position");
        for (int x : {1, 2, 3, 0})
            settle({original.x + x * 1000, original.y});
        motion.position = original;
        motion.travel = 0;
        for (int step = 0; step < 100; ++step)
        {
            motion.advance(.05);
            manager.tick(motion.position);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        settle(motion.position);
        requests = manager.stats().requests;
        unloaded = manager.stats().unloaded;
        cancellations = manager.stats().cancellations;
        check(unloaded >= 9, "Movement unloads previous tiles");
        check(manager.stats().failures == 0, "Actual motion route has no loading failures");
        check(service->stats().residentCells <= 1 && field->sourceCellCount() <= terrain.maxSourceCells,
              "Source and terrain contexts bounded");
        manager.clear();
        check(scene->getNumChildren() == 0 && manager.stats().activeMeshBytes == 0,
              "Scene ownership released");
    }
    for (const auto& p : observed)
        check(!p.valid(), "No old scene node retained after route");
#endif
    for (int x = 0; x < 20; ++x)
        value(provider->request({"quota:" + std::to_string(x), {grid.id, 20000 + x, 20000}, {"land"}}));
    std::size_t diskEntries = 0;
    for (const auto& entry : fs::directory_iterator(options.cacheDirectory))
        if (entry.is_directory() && fs::exists(entry.path() / "commit.json"))
            ++diskEntries;
    check(diskEntries <= 12 && provider->stats().diskEvictions > 0,
          "Disk cache remains bounded over route history");
    fs::create_directories(root / "output/vgds");
    J report = {{"checks", checks},
                {"catalog_cells", catalog->entries().size()},
                {"cold_ms", latency[0]},
                {"warm_ms", J::array({latency[1], latency[2], latency[3]})},
                {"disk_entries", diskEntries},
                {"disk_evictions", provider->stats().diskEvictions},
                {"source_loads", provider->stats().cellLoads},
                {"source_evictions", provider->stats().cellEvictions},
                {"scene_requests", requests},
                {"unloaded", unloaded},
                {"cancellations", cancellations},
                {"peak_mesh_cpu_bytes", peakMesh}};
#ifdef VEYLAXIS_HAS_OSGB
    const auto name = "s7-validation.json";
#else
    const auto name = "s7-validation-no-osgb.json";
#endif
    std::ofstream output(root / "output/vgds" / name);
    output << report.dump(2);
    std::cout << "VGDS S7: " << checks << " checks passed; requests=" << requests << " unloaded=" << unloaded
              << " disk entries=" << diskEntries << "\n";
    return 0;
}
catch (const std::exception& e)
{
    std::cerr << "S7 acceptance failed: " << e.what() << '\n';
    return 1;
}
