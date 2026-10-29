// 【阅读导航】数据服务扩展点：Dataset 是完整源数据，TerrainSource 是可采样高度函数，ServiceConfig 控制缓存与构建器。
#pragma once
#include "ITileProvider.hpp"
#include <functional>
#include <memory>
#include <stdexcept>

namespace veylaxis::vgds
{
class RequestCancelled : public std::runtime_error
{
  public:
    RequestCancelled() : std::runtime_error("Request cancelled or deadline exceeded")
    {
    }
};

class RequestScope
{
  public:
    explicit RequestScope(std::function<bool()>);
    ~RequestScope();

  private:
    std::function<bool()> previous_;
};

void requestCheckpoint();

struct TerrainSource
{
    std::string verticalDatum, algorithmVersion;
    Provenance provenance{Provenance::Estimated};
    std::function<std::optional<double>(XY)> elevation;
};

struct Dataset
{
    std::string id, version;
    GridSpec grid;
    Provenance provenance{Provenance::Synthetic};
    std::vector<Feature> features; // immutable source polygons/lines in global metres
    std::optional<TerrainSource> terrainSource;
    std::vector<std::string> supportedLayers{"land", "coastline"};
};

Result<TerrainData> sampleTerrain(const GridSpec&, const TileID&, double spacing, const TerrainSource&);
Dataset islandFixture(const GridSpec& grid);
double polygonArea(const Geometry& geometry);
// Boolean intersection uses the installed GDAL/GEOS only in its implementation.
Result<std::vector<Feature>> clipFeature(const Feature&, const Bounds&, const TileID&);
Result<std::vector<Feature>> intersectFeature(const Feature&, const Geometry& mask, const Bounds&,
                                              const TileID&);
Bounds geometryBounds(const Geometry&);

struct CacheStats
{
    std::uint64_t requests{}, hits{}, generations{}, repairs{};
};

using ArtifactBuilder = std::function<RenderArtifact(const TilePackage&, const std::string& outputPath)>;

struct ServiceConfig
{
    std::string cacheDirectory;
    bool osgbEnabled{false};
    std::string builderVersion;
    ArtifactBuilder builder;
    std::function<void(const Dataset&)> sourceLoaded;
    std::function<void(const std::string&)> sourceEvicted;
};

Result<std::string> requestProviderJson(ITileProvider&, const std::string&);

// Serializes concurrent requests; callers never access GIS state concurrently.
class LocalService final : public ITileProvider
{
  public:
    LocalService(Dataset dataset, ServiceConfig config);
    ~LocalService();
    TileResult request(const TileRequest&) override;
    Result<std::string> requestJson(const std::string& request);
    CacheStats stats() const;
    const Dataset& dataset() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// SHA-256, implemented with the Windows system crypto library, no downloaded dependency.
std::string contentHash(const std::string& bytes);
std::string fileHash(const std::string& path);
}
