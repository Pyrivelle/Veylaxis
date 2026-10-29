#include "veylaxis/streaming/TileManager.hpp"
#include "veylaxis/vgds/ITileProvider.hpp"
#include "veylaxis/vgds/Service.hpp"
#include "veylaxis/vgds/OsgBuilder.hpp"
#include "veylaxis/vgds/Codec.hpp"
#include <osg/observer_ptr>
#include <osg/MatrixTransform>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <thread>
#include <stdexcept>

namespace
{
using namespace veylaxis::vgds;
using namespace veylaxis::streaming;
using Clock = std::chrono::steady_clock;
int checks;

void check(bool b, const char* message)
{
    ++checks;
    if (!b)
        throw std::runtime_error(message);
}

template <class T> T value(Result<T> r)
{
    if (auto e = std::get_if<TileError>(&r))
        throw std::runtime_error(e->message);
    return std::get<T>(std::move(r));
}

void settle(TileManager& manager, XY position, std::size_t expected = 9)
{
    auto deadline = Clock::now() + std::chrono::seconds(20);
    for (;;)
    {
        check(!manager.tick(position), "Tick request");
        auto s = manager.stats();
        check(s.active <= 9 && s.loading <= 9 && s.queued <= 9 && s.completed <= 9 && s.inFlight <= 1,
              "Bounded work and active tiles");
        if (s.active == expected && s.loading == 0 && s.queued == 0 && s.completed == 0 && s.inFlight == 0)
            return;
        if (Clock::now() > deadline)
            throw std::runtime_error("Tile settle timeout");
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

LoadedTile dummy(const TileRequest& r, const GridSpec& g)
{
    MockProvider provider(g);
    return {value(provider.request(r)), new osg::Group, 128};
}
}
int runVgdsStreamingTests()
try
{
    checks = 0;
    const GridSpec grid{"stream-test", "EPSG:32619", {503500, 4856500}, 1000};
    XY initial{504000, 4857000};
    auto provider = std::make_shared<MockProvider>(grid);
    TileClient client(
        [provider](const std::string& bytes) -> Result<std::string>
        {
            auto r = value(decodeRequestJson(bytes));
            return encodeJson(value(provider->request(r)));
        });
    TileRequest request{"correlation", {grid.id, 0, 0}, {"land"}};
    check(std::holds_alternative<TilePackage>(client.request(request)), "TileClient request/response");
    TileClient mismatch(
        [provider](const std::string& bytes) -> Result<std::string>
        {
            auto r = value(decodeRequestJson(bytes));
            r.requestId = "wrong";
            return encodeJson(value(provider->request(r)));
        });
    check(std::holds_alternative<TileError>(mismatch.request(request)), "Reject mismatched response");
    TileClient malformed(
        [](const std::string&) -> Result<std::string>
        {
            return std::string("bad");
        });
    check(std::holds_alternative<TileError>(malformed.request(request)), "Reject malformed payload");
    TileClient throwing(
        [](const std::string&) -> Result<std::string>
        {
            throw std::runtime_error("offline");
        });
    check(std::holds_alternative<TileError>(throwing.request(request)), "Transport exception contained");
    osg::ref_ptr<osg::Group> root = new osg::Group;
    std::vector<osg::observer_ptr<osg::Node>> original;
    {
        TileManager manager(grid, root,
                            [&](const TileRequest& r) -> LoadResult
                            {
                                return dummy(r, grid);
                            });
        settle(manager, initial);
        check(root->getNumChildren() == 9, "Initial 3x3");
        auto requests = manager.stats().requests;
        for (int n = 0; n < 50; ++n)
            check(!manager.tick(initial), "Repeated position");
        check(manager.stats().requests == requests, "No duplicate request for active/loading tile");
        for (unsigned i = 0; i < root->getNumChildren(); ++i)
            original.emplace_back(root->getChild(i));
        settle(manager, {initial.x + 1000, initial.y});
        check(manager.stats().requests == requests + 3 && manager.stats().unloaded == 3,
              "One boundary adds/removes three tiles");
        settle(manager, {initial.x + 2000, initial.y});
        check(manager.stats().requests == requests + 6 && manager.stats().unloaded == 6,
              "Second boundary adds/removes three tiles");
        int released = 0;
        for (const auto& p : original)
            if (!p.valid())
                ++released;
        check(released == 6, "Unloaded scene nodes freed");
        for (int x = 3; x <= 24; ++x)
        {
            settle(manager, {initial.x + 1000 * x, initial.y});
            check(manager.stats().activeMeshBytes == 9 * 128, "Memory remains bounded during long route");
        }
        manager.clear();
        check(root->getNumChildren() == 0 && manager.stats().activeMeshBytes == 0,
              "Clear drops all active ownership");
        for (const auto& p : original)
            check(!p.valid(), "No retained history node");
        check(manager.tick({NAN, 0}).has_value(), "Invalid position fails safely");
        std::atomic<bool> rejected{false};
        std::thread other(
            [&]
            {
                try
                {
                    manager.tick(initial);
                }
                catch (const std::logic_error&)
                {
                    rejected = true;
                }
            });
        other.join();
        check(rejected, "Scene mutations restricted to owner thread");
    }
    // Deterministic in-flight stale response: move while first job is blocked.
    std::mutex mutex;
    std::condition_variable wake;
    bool entered = false, release = false;
    std::atomic<bool> block{true};
    {
        TileManager manager(grid, root,
                            [&](const TileRequest& r) -> LoadResult
                            {
                                if (block.exchange(false))
                                {
                                    std::unique_lock<std::mutex> lock(mutex);
                                    entered = true;
                                    wake.notify_one();
                                    wake.wait(lock,
                                              [&]
                                              {
                                                  return release;
                                              });
                                }
                                return dummy(r, grid);
                            });
        manager.tick(initial);
        {
            std::unique_lock<std::mutex> lock(mutex);
            if (!wake.wait_for(lock, std::chrono::seconds(5),
                               [&]
                               {
                                   return entered;
                               }))
                throw std::runtime_error("Worker did not start");
        }
        for (int i = 0; i < 10; ++i)
        {
            manager.tick({initial.x + 10000 + i * 1000, initial.y});
            check(manager.stats().queued <= 9, "Obsolete queued requests removed");
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            release = true;
        }
        wake.notify_one();
        settle(manager, {initial.x + 19000, initial.y});
        check(manager.stats().discarded >= 1, "Stale in-flight result discarded");
        check(root->getNumChildren() == 9, "No obsolete node attached");
        manager.clear();
    }
    // First load fails, second succeeds; terminal failure is bounded by maxAttempts.
    {
        std::atomic<int> calls{0};
        TileManager manager(
            grid, root,
            [&](const TileRequest& r) -> LoadResult
            {
                if (++calls == 1)
                    throw std::runtime_error("injected load failure");
                return dummy(r, grid);
            },
            ManagerConfig{std::chrono::milliseconds(1), 3});
        settle(manager, initial);
        check(manager.stats().failures == 1 && manager.stats().retries == 1,
              "Exception reported and retried");
        manager.clear();
    }
    {
        TileManager manager(
            grid, root,
            [](const TileRequest&) -> LoadResult
            {
                return TileError{ErrorCode::IoFailure, "missing"};
            },
            ManagerConfig{std::chrono::milliseconds(1), 2});
        auto deadline = Clock::now() + std::chrono::seconds(5);
        while (Clock::now() < deadline)
        {
            manager.tick(initial);
            auto s = manager.stats();
            if (s.failures == 18 && s.loading == 0)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        check(manager.stats().requests == 18 && manager.stats().failed == 9 && root->getNumChildren() == 0,
              "Terminal failures do not loop or crash");
        manager.clear();
    }
    // Actual embedded service + JSON + OSGB loader, followed by unload/destruction.
    auto cache =
        std::filesystem::temp_directory_path() /
        ("veylaxis-s3-" + contentHash(std::to_string(Clock::now().time_since_epoch().count())).substr(0, 16));
    ServiceConfig config;
    config.cacheDirectory = cache.string();
    config.osgbEnabled = true;
    config.builder = buildOsgb;
    config.builderVersion = OsgBuilderVersion;
    auto service = std::make_shared<LocalService>(islandFixture(grid), config);
    auto realClient = std::make_shared<TileClient>(
        [service](const std::string& s)
        {
            return service->requestJson(s);
        });
    std::vector<osg::observer_ptr<osg::Node>> realNodes;
    {
        TileManager manager(grid, root, makeLoader(realClient));
        settle(manager, initial);
        check(manager.stats().activeMeshBytes > 0, "Actual mesh memory observed");
        for (unsigned n = 0; n < root->getNumChildren(); ++n)
            realNodes.emplace_back(root->getChild(n)->asGroup()->getChild(0));
        auto firstBytes = manager.stats().activeMeshBytes;
        settle(manager, {initial.x + 2000, initial.y});
        settle(manager, initial);
        check(service->stats().hits >= 3, "Return path uses disk cache");
        check(manager.stats().activeMeshBytes == firstBytes, "Route returns to identical mesh memory");
    }
    check(root->getNumChildren() == 0, "Destructor unloads all tiles and joins worker");
    for (const auto& n : realNodes)
        check(!n.valid(), "Actual OSGB nodes not retained by global object cache");
    config.osgbEnabled = false;
    auto dataOnly = std::make_shared<LocalService>(islandFixture(grid), config);
    auto dataClient = std::make_shared<TileClient>(
        [dataOnly](const std::string& s)
        {
            return dataOnly->requestJson(s);
        });
    {
        TileManager manager(grid, root, makeLoader(dataClient));
        settle(manager, initial);
        check(manager.stats().activeMeshBytes > 0, "Runtime OSGB off builds semantic data in memory");
    }
    std::cout << "VGDS S3: " << checks
              << " checks passed; route 24 boundaries; active<=9; stale/error/release checks passed\n";
    return 0;
}
catch (const std::exception& e)
{
    std::cerr << "VGDS S3 failed: " << e.what() << '\n';
    return 1;
}
