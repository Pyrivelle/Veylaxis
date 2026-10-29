// 【阅读导航】可选 OSG 边界：此接口允许 OSG 构建依赖，但不会把 OSG 类型加入 VGDS Core 的数据契约。
#pragma once
#include "Types.hpp"
#include "IslandTerrain.hpp"

namespace veylaxis::vgds
{
inline constexpr const char* OsgBuilderVersion = "flat-estimated-v1-height12-base0";
// Deliberately STL-only even at this optional renderer boundary.
RenderArtifact buildOsgb(const TilePackage&, const std::string& outputPath);
ArtifactBuilder proceduralBuilder(std::shared_ptr<IslandTerrain>);
}
