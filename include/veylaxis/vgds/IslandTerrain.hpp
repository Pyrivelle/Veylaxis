// 【阅读导航】岛屿高度场接口：按来源 ID、版本和全局位置取样，保证相邻 Tile 使用同一高度依据。
#pragma once
#include "Service.hpp"
#include <array>

namespace veylaxis::vgds
{
struct IslandTerrainConfig
{
    bool enabled{false};
    double heightMeters{42}, shoreScaleMeters{200}, slopeExponent{1.25}, sampleSpacingMeters{25},
        normalStepMeters{0.5}, baseDepthMeters{12};
    std::size_t maxSourceCells{16};
};

struct IslandSample
{
    double elevation{};
    std::array<double, 3> normal{0, 0, 1};
};

// Full, uncut source coasts. No OSG/GDAL types, no tile-local normalization.
class IslandTerrain
{
  public:
    explicit IslandTerrain(IslandTerrainConfig);
    ~IslandTerrain();
    void addDataset(const Dataset&);
    std::optional<IslandSample> sample(const std::string& sourceId, const std::string& version, XY) const;
    std::size_t sourceCellCount() const;
    const IslandTerrainConfig& config() const;
    std::string version() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
