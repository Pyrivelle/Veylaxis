// 【阅读导航】最小接口测试数据源：验证请求/响应契约；完整模拟岛屿的裁剪测试使用 IslandFixture + LocalService。
#include "veylaxis/vgds/ITileProvider.hpp"
#include "veylaxis/vgds/Grid.hpp"
#include <stdexcept>
#include <utility>
#include <cmath>
#include <limits>

namespace veylaxis::vgds
{
MockProvider::MockProvider(GridSpec grid) : grid_(std::move(grid))
{
    if (auto e = validate(grid_))
        throw std::invalid_argument(e->message);
}

TileResult MockProvider::request(const TileRequest& r)
{
    if (r.schemaVersion != SchemaVersion)
        return TileError{ErrorCode::UnsupportedVersion, "Unsupported request schema"};
    if (!std::isfinite(r.terrainSpacingMeters) || r.terrainSpacingMeters <= 0)
        return TileError{ErrorCode::InvalidResolution, "Spacing must be finite and positive"};
    auto bounds = boundsOf(grid_, r.tile);
    if (auto e = std::get_if<TileError>(&bounds))
        return *e;
    auto layers = normalizeLayers(r.layers);
    if (auto e = std::get_if<TileError>(&layers))
        return *e;
    TilePackage p;
    auto& m = p.metadata;
    m.requestId = r.requestId;
    m.datasetId = "vgds-s1-mock";
    m.dataVersion = "1";
    m.algorithmVersion = "fixed-fixture-1";
    m.grid = grid_;
    m.tile = r.tile;
    m.bounds = std::get<Bounds>(bounds);
    m.tileOrigin = {m.bounds.minX, m.bounds.minY};
    for (const auto& layer : std::get<std::vector<std::string>>(layers))
    {
        if (layer == "land" || layer == "coastline")
            m.layers[layer] = LayerState::Empty;
        else if (layer == "terrain")
        {
            auto count = nodeCount(grid_, r.terrainSpacingMeters);
            if (auto e = std::get_if<TileError>(&count))
                return *e;
            TerrainData t;
            t.columns = t.rows = std::get<std::uint32_t>(count);
            t.spacingMeters = r.terrainSpacingMeters;
            t.northWestSample = {m.bounds.minX, m.bounds.maxY};
            t.verticalDatum = "synthetic-demo-zero";
            const std::size_t size = static_cast<std::size_t>(t.columns) * t.rows;
            t.elevations.assign(size, -10.0f);
            t.valid.assign(size, 1);
            // One deliberate missing sample verifies unknown != zero sea level.
            t.valid[size / 2] = 0;
            t.elevations[size / 2] = std::numeric_limits<float>::quiet_NaN();
            p.terrain = std::move(t);
            m.layers[layer] = LayerState::Available;
        }
        else if (layer == "navigation_aids")
            m.layers[layer] = LayerState::NoCoverage;
        else
            m.layers[layer] = LayerState::Unsupported;
    }
    return p;
}
}
