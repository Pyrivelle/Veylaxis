// 【阅读导航】网格接口：调用者提供 CRS、原点和区块边长；核心层把 CRS 当标识而不引入 GIS SDK。
#pragma once
#include "Types.hpp"

namespace veylaxis::vgds
{
Result<TileID> locate(const GridSpec& grid, XY position);
Result<Bounds> boundsOf(const GridSpec& grid, const TileID& tile);
Result<std::uint32_t> nodeCount(const GridSpec& grid, double spacingMeters);
Result<std::vector<std::string>> normalizeLayers(const std::vector<std::string>& layers);
std::optional<TileError> validate(const GridSpec& grid);
std::optional<TileError> validate(const TilePackage& package);
}
