// 【阅读导航】渲染层模型注册表：规则匹配与共享原型缓存；每个物标只共享资源，不共享位置变换。
#pragma once
#include "veylaxis/vgds/Types.hpp"
#include <osg/Group>
#include <functional>
#include <memory>

namespace veylaxis::streaming
{
struct ModelRegistryStats
{
    std::uint64_t diskLoads{}, cacheHits{}, instances{}, duplicates{}, skipped{}, warnings{}, evictions{};
    std::size_t cachedModels{};
};

// Renderer-only. Each instance owns its transform; loaded immutable model nodes
// are shared. Point positions remain projected metres, Z is configured explicitly.
class ModelRegistry
{
  public:
    using WarningSink = std::function<void(const std::string&)>;
    explicit ModelRegistry(std::string configurationPath, WarningSink = {});
    ~ModelRegistry();
    bool reload(); // active nodes keep their existing assets until their Tile unloads
    osg::ref_ptr<osg::Node> loadModel(const std::string& path);
    std::optional<std::string> matchingRule(const veylaxis::vgds::Feature&) const;
    osg::ref_ptr<osg::Group> instantiate(const veylaxis::vgds::TilePackage&);
    ModelRegistryStats stats() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
