// 【阅读导航】区块生命周期管理：工作线程只加载数据/节点，主场景线程 tick() 才挂接和移除节点。
#include "veylaxis/streaming/TileManager.hpp"
#include "veylaxis/vgds/Grid.hpp"
#include <osg/MatrixTransform>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <limits>
#include <stdexcept>

namespace veylaxis::streaming
{
using namespace veylaxis::vgds;
using Key = std::pair<std::int64_t, std::int64_t>;
using Clock = std::chrono::steady_clock;

struct TileManager::Impl
{
    GridSpec grid;
    osg::ref_ptr<osg::Group> root;
    Loader loader;
    ManagerConfig config;
    std::thread::id owner;
    std::string requestPrefix;

    std::string id(std::uint64_t ticket) const
    {
        return requestPrefix + ":" + std::to_string(ticket);
    }

    struct Entry
    {
        TileState state{TileState::Unloaded};
        std::uint64_t ticket{}, bytes{};
        unsigned attempts{};
        Clock::time_point retry;
        osg::ref_ptr<osg::Node> node;
    };

    struct Job
    {
        Key key;
        std::uint64_t ticket;
    };

    struct Completion
    {
        Job job;
        LoadResult result;
        double milliseconds;
    };

    std::map<Key, Entry> entries;
    std::uint64_t nextTicket{};
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::deque<Job> jobs;
    std::deque<Completion> completed;
    std::set<std::uint64_t> wanted;
    bool stopping{};
    std::thread worker;
    ManagerStats counters;

    void checkOwner() const
    {
        if (std::this_thread::get_id() != owner)
            throw std::logic_error("TileManager requires scene/update thread");
    }

    // 单工作线程从队列取任务；结果只进入完成队列，不能在这里修改已显示的场景树。
    void run()
    {
        for (;;)
        {
            Job job;
            {
                std::unique_lock<std::mutex> lock(mutex);
                wake.wait(lock,
                          [&]
                          {
                              return stopping || !jobs.empty();
                          });
                if (stopping)
                    return;
                job = jobs.front();
                jobs.pop_front();
                ++counters.requests;
                counters.inFlight = 1;
            }
            const auto start = Clock::now();
            LoadResult result = TileError{ErrorCode::GenerationFailure, "Loader failed"};
            try
            {
                result = loader(TileRequest{id(job.ticket),
                                            {grid.id, job.key.first, job.key.second},
                                            config.layers,
                                            config.terrainSpacingMeters});
            }
            catch (const std::exception& e)
            {
                result = TileError{ErrorCode::GenerationFailure, e.what()};
            }
            catch (...)
            {
                result = TileError{ErrorCode::GenerationFailure, "Unknown loader exception"};
            }
            double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
            std::lock_guard<std::mutex> guard(mutex);
            counters.inFlight = 0;
            if (stopping)
                return;
            if (!wanted.count(job.ticket))
            {
                ++counters.discarded;
                continue;
            }
            completed.push_back({job, std::move(result), ms});
        }
    }
};

TileManager::TileManager(GridSpec grid, osg::Group* root, Loader loader, ManagerConfig config)
    : impl_(std::make_unique<Impl>())
{
    if (auto e = validate(grid))
        throw std::invalid_argument(e->message);
    if (!root || !loader || config.maxAttempts == 0 || config.maxAttempts > 10 ||
        config.retryDelay.count() < 0)
        throw std::invalid_argument("Invalid TileManager configuration");
    if (auto names = normalizeLayers(config.layers); std::holds_alternative<TileError>(names))
        throw std::invalid_argument("Invalid manager layers");
    auto& i = *impl_;
    i.grid = std::move(grid);
    i.root = root;
    i.loader = std::move(loader);
    i.config = config;
    i.owner = std::this_thread::get_id();
    i.requestPrefix = std::to_string(Clock::now().time_since_epoch().count()) + "-" +
                      std::to_string(reinterpret_cast<std::uintptr_t>(this));
    i.worker = std::thread(
        [&i]
        {
            i.run();
        });
}

TileManager::~TileManager()
{
    auto& i = *impl_;
    {
        std::lock_guard<std::mutex> guard(i.mutex);
        for (const auto& e : i.entries)
            if (e.second.state == TileState::Loading && i.config.cancelRequest)
                i.config.cancelRequest(i.id(e.second.ticket));
        i.stopping = true;
        i.jobs.clear();
        i.wanted.clear();
    }
    i.wake.notify_one();
    if (i.worker.joinable())
        i.worker.join();
    // Destruction follows the same owning-thread contract as scene mutation.
    for (auto& e : i.entries)
        if (e.second.node)
            i.root->removeChild(e.second.node);
}

// 主线程状态机：移除不再需要的块 → 消费匹配任务号的结果 → 安排缺少的块或重试。
std::optional<TileError> TileManager::tick(XY position)
{
    auto& i = *impl_;
    i.checkOwner();
    auto located = locate(i.grid, position);
    if (auto e = std::get_if<TileError>(&located))
        return *e;
    auto center = std::get<TileID>(located);
    if (center.x == INT64_MIN || center.x == INT64_MAX || center.y == INT64_MIN || center.y == INT64_MAX)
        return TileError{ErrorCode::InvalidRequest, "5x5 neighborhood index overflow"};
    std::set<Key> desired;
    for (int y = -2; y <= 2; ++y)
        for (int x = -2; x <= 2; ++x)
        {
            TileID t{i.grid.id, center.x + x, center.y + y};
            auto b = boundsOf(i.grid, t);
            if (auto e = std::get_if<TileError>(&b))
                return *e;
            desired.emplace(t.x, t.y);
        }
    std::lock_guard<std::mutex> guard(i.mutex);
    for (auto entry = i.entries.begin(); entry != i.entries.end();)
    {
        if (desired.count(entry->first))
        {
            ++entry;
            continue;
        }
        i.wanted.erase(entry->second.ticket);
        if (entry->second.state == TileState::Loading && i.config.cancelRequest)
        {
            i.config.cancelRequest(i.id(entry->second.ticket));
            ++i.counters.cancellations;
        }
        if (entry->second.node)
        {
            i.root->removeChild(entry->second.node);
            ++i.counters.unloaded;
        }
        entry = i.entries.erase(entry);
    }
    for (auto job = i.jobs.begin(); job != i.jobs.end();)
        if (!i.wanted.count(job->ticket))
            job = i.jobs.erase(job);
        else
            ++job;
    // 任务号可区分旧位置请求与新请求；已经过期的完成结果应丢弃，避免错误挂入场景。
    for (auto& c : i.completed)
    {
        auto entry = i.entries.find(c.job.key);
        if (entry == i.entries.end() || entry->second.ticket != c.job.ticket)
        {
            ++i.counters.discarded;
            continue;
        }
        auto& e = entry->second;
        i.wanted.erase(e.ticket);
        i.counters.lastLoadMilliseconds = c.milliseconds;
        auto* loaded = std::get_if<LoadedTile>(&c.result);
        const auto sameGrid = [&]
        {
            if (!loaded)
                return false;
            const auto& g = loaded->package.metadata.grid;
            return g.id == i.grid.id && g.horizontalCrs == i.grid.horizontalCrs &&
                   g.horizontalUnit == i.grid.horizontalUnit && g.origin.x == i.grid.origin.x &&
                   g.origin.y == i.grid.origin.y && g.tileSizeMeters == i.grid.tileSizeMeters;
        };
        if (loaded && loaded->node && sameGrid() &&
            loaded->package.metadata.tile == TileID{i.grid.id, c.job.key.first, c.job.key.second} &&
            loaded->package.metadata.requestId == i.id(c.job.ticket))
        {
            e.state = TileState::Ready;
            auto origin = loaded->package.metadata.tileOrigin;
            osg::ref_ptr<osg::MatrixTransform> placement = new osg::MatrixTransform;
            placement->setName("tile:" + std::to_string(c.job.key.first) + "," +
                               std::to_string(c.job.key.second));
            placement->setMatrix(
                osg::Matrixd::translate(origin.x - i.grid.origin.x, origin.y - i.grid.origin.y, 0));
            placement->addChild(loaded->node);
            e.node = placement;
            e.bytes = loaded->meshBytes;
            i.root->addChild(e.node);
            e.state = TileState::Active;
            ++i.counters.loaded;
        }
        else
        {
            e.state = TileState::Failed;
            e.retry = Clock::now() + i.config.retryDelay;
            ++i.counters.failures;
        }
    }
    i.completed.clear();
    for (auto key : desired)
    {
        auto& e = i.entries[key];
        if (e.state == TileState::Unloaded ||
            (e.state == TileState::Failed && e.attempts < i.config.maxAttempts && Clock::now() >= e.retry))
        {
            if (e.attempts)
                ++i.counters.retries;
            ++e.attempts;
            e.ticket = ++i.nextTicket;
            e.state = TileState::Loading;
            i.wanted.insert(e.ticket);
            i.jobs.push_back({key, e.ticket});
        }
    }
    i.wake.notify_one();
    return {};
}

// 清理活动节点和待处理队列；执行中的旧任务在后续完成时也不能重新挂接。
void TileManager::clear()
{
    auto& i = *impl_;
    i.checkOwner();
    std::lock_guard<std::mutex> guard(i.mutex);
    i.wanted.clear();
    i.jobs.clear();
    i.completed.clear();
    for (const auto& e : i.entries)
        if (e.second.state == TileState::Loading && i.config.cancelRequest)
        {
            i.config.cancelRequest(i.id(e.second.ticket));
            ++i.counters.cancellations;
        }
    for (auto& e : i.entries)
        if (e.second.node)
        {
            i.root->removeChild(e.second.node);
            ++i.counters.unloaded;
        }
    i.entries.clear();
}

ManagerStats TileManager::stats() const
{
    auto& i = *impl_;
    i.checkOwner();
    std::lock_guard<std::mutex> guard(i.mutex);
    auto s = i.counters;
    s.queued = i.jobs.size();
    s.completed = i.completed.size();
    for (const auto& e : i.entries)
    {
        if (e.second.state == TileState::Active)
        {
            ++s.active;
            s.activeMeshBytes += e.second.bytes;
        }
        else if (e.second.state == TileState::Loading)
            ++s.loading;
        else if (e.second.state == TileState::Failed)
            ++s.failed;
    }
    for (const auto& c : i.completed)
        if (auto p = std::get_if<LoadedTile>(&c.result))
            s.pendingMeshBytes += p->meshBytes;
    return s;
}
}
