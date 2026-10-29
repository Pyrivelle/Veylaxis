// 【阅读导航】数据到场景的适配层：经 TileClient 取得 TilePackage，读取静态缓存并附加航标实例。
#include "veylaxis/streaming/TileManager.hpp"
#include "veylaxis/streaming/ModelRegistry.hpp"
#include "veylaxis/vgds/Service.hpp"
#include "../vgds/OsgMesh.hpp"
#include <osg/Geode>
#include <osg/Geometry>
#include <osg/ValueObject>
#include <osgDB/ReadFile>
#include <osgDB/Options>
#include <filesystem>
#include <stdexcept>

namespace veylaxis::streaming
{
using namespace veylaxis::vgds;

Loader makeLoader(std::shared_ptr<TileClient> client, std::shared_ptr<IslandTerrain> field,
                  std::shared_ptr<ModelRegistry> models)
{
    if (!client)
        throw std::invalid_argument("Missing TileClient");
    return [client = std::move(client), field = std::move(field),
            models = std::move(models)](const TileRequest& r) -> LoadResult
    {
        auto result = client->request(r);
        if (auto e = std::get_if<TileError>(&result))
            return *e;
        auto package = std::get<TilePackage>(std::move(result));
        osg::ref_ptr<osg::Node> node;
        // 有文件缓存时先校验哈希、大小及坐标元数据；关闭 OSGB 输出则从语义数据在内存建网格。
        if (!package.artifacts.empty())
        {
            const auto& a = package.artifacts.front();
            if (std::filesystem::file_size(a.path) != a.bytes || fileHash(a.path) != a.contentHash)
                return TileError{ErrorCode::InvalidPackage, "OSGB integrity mismatch"};
            osg::ref_ptr<osgDB::Options> options = new osgDB::Options;
            options->setObjectCacheHint(osgDB::Options::CACHE_NONE);
            node = osgDB::readRefNodeFile(a.path, options);
            if (!node)
                return TileError{ErrorCode::IoFailure, "OSGB loading failed"};
            double x = 0, y = 0, z = 0;
            if (!node->getUserValue("origin_easting", x) || !node->getUserValue("origin_northing", y) ||
                !node->getUserValue("vertical_scale", z) || x != a.origin.x || y != a.origin.y || z != 1)
                return TileError{ErrorCode::InvalidPackage, "OSGB coordinate metadata mismatch"};
        }
        else
            node = createLandNode(package, field); // runtime OSGB off: build in memory only
        // Point models belong to the application, never the static OSGB Builder.
        osg::ref_ptr<osg::Group> tile = new osg::Group;
        tile->addChild(node);
        node = tile;
        // 航标挂在所属 Tile 下，卸载父块即释放实例；模型原型仍可由 Registry 缓存复用。
        if (models)
            tile->addChild(models->instantiate(package));
        struct Memory : osg::NodeVisitor
        {
            std::uint64_t bytes{};
            Memory() : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
            {
            }
            void apply(osg::Geode& geode) override
            {
                for (unsigned i = 0; i < geode.getNumDrawables(); ++i)
                    if (auto* g = geode.getDrawable(i)->asGeometry())
                    {
                        for (auto* a : {g->getVertexArray(), g->getNormalArray(), g->getColorArray()})
                            if (a)
                                bytes += a->getTotalDataSize();
                        for (unsigned k = 0; k < g->getNumPrimitiveSets(); ++k)
                            if (auto* indices = g->getPrimitiveSet(k)->getDrawElements())
                                bytes += indices->getTotalDataSize();
                    }
            }
        } memory;
        node->accept(memory);
        return LoadedTile{std::move(package), node, memory.bytes};
    };
}
}
