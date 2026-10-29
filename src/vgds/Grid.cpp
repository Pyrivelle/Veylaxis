// 【阅读导航】纯数据网格计算：Tile 编号、地理边界和采样节点数，不执行经纬度投影。
#include "veylaxis/vgds/Grid.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace veylaxis::vgds
{
std::optional<TileError> validate(const GridSpec& g)
{
    if (g.id.empty() || g.horizontalCrs.empty() || g.id.size() > 1048576 ||
        g.horizontalCrs.size() > 1048576 || g.horizontalUnit != "m" || !std::isfinite(g.origin.x) ||
        !std::isfinite(g.origin.y) || !std::isfinite(g.tileSizeMeters) || g.tileSizeMeters <= 0)
        return TileError{ErrorCode::InvalidGrid,
                         "Grid requires identity, projected metre CRS, finite origin and positive size"};
    return {};
}

Result<Bounds> boundsOf(const GridSpec& g, const TileID& t)
{
    if (auto error = validate(g))
        return *error;
    if (t.gridId != g.id)
        return TileError{ErrorCode::InvalidGrid, "Tile belongs to another grid"};
    const long double x =
        static_cast<long double>(g.origin.x) + static_cast<long double>(t.x) * g.tileSizeMeters;
    const long double y =
        static_cast<long double>(g.origin.y) + static_cast<long double>(t.y) * g.tileSizeMeters;
    Bounds b{static_cast<double>(x), static_cast<double>(y), static_cast<double>(x + g.tileSizeMeters),
             static_cast<double>(y + g.tileSizeMeters)};
    if (!std::isfinite(b.minX) || !std::isfinite(b.minY) || !std::isfinite(b.maxX) ||
        !std::isfinite(b.maxY) || b.minX >= b.maxX || b.minY >= b.maxY)
        return TileError{ErrorCode::InvalidGrid, "Tile coordinates exceed double spatial precision"};
    return b;
}

// 用 floor 而非整数截断计算编号，原点西侧/南侧的负编号同样成立；点采用左闭右开归属。
Result<TileID> locate(const GridSpec& g, XY p)
{
    if (auto error = validate(g))
        return *error;
    if (!std::isfinite(p.x) || !std::isfinite(p.y))
        return TileError{ErrorCode::InvalidRequest, "Position must be finite"};
    auto index = [&](double value, double origin) -> std::optional<std::int64_t>
    {
        const long double v = std::floor((static_cast<long double>(value) - origin) / g.tileSizeMeters);
        // 2^63 is an exact exclusive upper bound even where long double == double.
        if (v < -9223372036854775808.0L || v >= 9223372036854775808.0L)
            return {};
        return static_cast<std::int64_t>(v);
    };
    auto x = index(p.x, g.origin.x), y = index(p.y, g.origin.y);
    if (!x || !y)
        return TileError{ErrorCode::InvalidRequest, "Tile index overflow"};
    TileID t{g.id, *x, *y};
    auto b = boundsOf(g, t);
    if (auto e = std::get_if<TileError>(&b))
        return *e;
    return t;
}

Result<std::uint32_t> nodeCount(const GridSpec& g, double spacing)
{
    if (auto error = validate(g))
        return *error;
    if (!std::isfinite(spacing) || spacing <= 0)
        return TileError{ErrorCode::InvalidResolution, "Spacing must be positive"};
    const double n = g.tileSizeMeters / spacing;
    if (!std::isfinite(n) || n < 1 || n > 1023 || std::abs(n - std::round(n)) > 1e-9)
        return TileError{ErrorCode::InvalidResolution,
                         "Spacing must divide tile size; at most 1024 nodes per side"};
    return static_cast<std::uint32_t>(std::round(n)) + 1;
}

Result<std::vector<std::string>> normalizeLayers(const std::vector<std::string>& layers)
{
    if (layers.empty() || layers.size() > 64)
        return TileError{ErrorCode::InvalidRequest, "Request needs 1..64 layer names"};
    auto result = layers;
    for (const auto& s : result)
        if (s.empty() || s.size() > 64 ||
            !std::all_of(s.begin(), s.end(),
                         [](unsigned char c)
                         {
                             return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
                         }))
            return TileError{ErrorCode::InvalidRequest, "Layer names use lowercase ASCII identifiers"};
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}
}
