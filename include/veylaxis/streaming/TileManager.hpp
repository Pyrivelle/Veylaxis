// 【阅读导航】先看状态与统计，再读实现：负责活动 3×3 和异步任务，场景修改必须在拥有它的线程进行。
#pragma once
#include "veylaxis/vgds/TileClient.hpp"
#include "veylaxis/vgds/IslandTerrain.hpp"
#include <osg/Group>
#include <chrono>
#include <functional>
#include <memory>

namespace veylaxis::streaming
{
enum class TileState
{
    Unloaded,
    Loading,
    Ready,
    Active,
    Failed
};

struct LoadedTile
{
    veylaxis::vgds::TilePackage package;
    osg::ref_ptr<osg::Node> node;
    std::uint64_t meshBytes{}; // CPU mesh arrays/index bytes; not total process RSS
};

using LoadResult = std::variant<LoadedTile, veylaxis::vgds::TileError>;
using Loader = std::function<LoadResult(const veylaxis::vgds::TileRequest&)>;

struct ManagerConfig
{
    std::chrono::milliseconds retryDelay{100};
    unsigned maxAttempts{3};
    std::vector<std::string> layers{"land"};
    double terrainSpacingMeters{10};
    std::function<void(const std::string&)> cancelRequest;
};

struct ManagerStats
{
    std::size_t active{}, loading{}, failed{}, queued{}, completed{}, inFlight{};
    std::uint64_t requests{}, loaded{}, unloaded{}, discarded{}, failures{}, retries{}, activeMeshBytes{},
        pendingMeshBytes{}, cancellations{};
    double lastLoadMilliseconds{};
};

class TileManager
{
  public:
    TileManager(veylaxis::vgds::GridSpec, osg::Group* tileRoot, Loader, ManagerConfig = {});
    ~TileManager();
    // Call only on the owning scene/update thread, including clear/destruction.
    std::optional<veylaxis::vgds::TileError> tick(veylaxis::vgds::XY position);
    void clear();
    ManagerStats stats() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
// Used by both real and mock providers. No GIS parsing in the scene client.
class ModelRegistry;
Loader makeLoader(std::shared_ptr<veylaxis::vgds::TileClient>,
                  std::shared_ptr<veylaxis::vgds::IslandTerrain> field = {},
                  std::shared_ptr<ModelRegistry> models = {});
}
