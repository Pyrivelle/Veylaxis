// 【阅读导航】动态海底数据层：以 DEPARE 限定范围，构建深度估算函数，再为 Tile 采样高程和法线。
#include "veylaxis/vgds/Bathymetry.hpp"
#include "veylaxis/vgds/Grid.hpp"
#include "PreparedShape.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <tuple>
#include <set>
#include <stdexcept>

namespace veylaxis::vgds
{
namespace
{
int inRing(XY p, const Ring& ring)
{
    bool inside = false;
    for (std::size_t i = 0, j = ring.points.size() - 1; i < ring.points.size(); j = i++)
    {
        auto a = ring.points[j], b = ring.points[i];
        if (veylaxis::segmentDistance({p.x, p.y}, {{a.x, a.y}, {b.x, b.y}, 0}) < 1e-7)
            return 0;
        if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x)
            inside = !inside;
    }
    return inside ? 1 : -1;
}

bool contains(XY p, const Geometry& g)
{
    auto polygon = [&](const Polygon& v)
    {
        if (inRing(p, v.outer) < 0)
            return false;
        for (const auto& h : v.holes)
            if (inRing(p, h) > 0)
                return false;
        return true;
    };
    if (auto v = std::get_if<Polygon>(&g))
        return polygon(*v);
    if (auto m = std::get_if<MultiPolygon>(&g))
        for (const auto& v : m->polygons)
            if (polygon(v))
                return true;
    return false;
}

using EdgeKey = std::array<long long, 4>;

EdgeKey key(XY a, XY b)
{
    if (std::tie(a.x, a.y) > std::tie(b.x, b.y))
        std::swap(a, b);
    return {std::llround(a.x * 1000), std::llround(a.y * 1000), std::llround(b.x * 1000),
            std::llround(b.y * 1000)};
}

struct Model
{
    struct Area
    {
        DepthAreaInput source;
        Bounds bounds;
        std::shared_ptr<veylaxis::LinearContourReconstructor> algorithm;
        std::shared_ptr<PreparedShape> prepared;
    };

    std::vector<Area> areas;

    std::optional<std::size_t> areaAt(XY p) const
    {
        for (std::size_t i = 0; i < areas.size(); ++i)
        {
            const auto& a = areas[i];
            if (p.x >= a.bounds.minX && p.x <= a.bounds.maxX && p.y >= a.bounds.minY &&
                p.y <= a.bounds.maxY && a.prepared->intersects(p))
                return i;
        }
        return {};
    }
};
}

// 先给每个深度区域选择相邻等深线，再关联落在有效深度带内的测深点；输出时水深取负变成高程。
TerrainSource makeSeabedSource(SeabedInput input)
{
    if (input.verticalDatum.empty())
        throw std::invalid_argument("Missing sounding datum");
    auto model = std::make_shared<Model>();
    std::sort(input.areas.begin(), input.areas.end(),
              [](const auto& a, const auto& b)
              {
                  return a.sourceId < b.sourceId;
              });
    std::map<EdgeKey, std::vector<std::size_t>> contourEdges;
    for (std::size_t i = 0; i < input.contours.size(); ++i)
    {
        const auto& s = input.contours[i];
        contourEdges[key({s.a.x, s.a.y}, {s.b.x, s.b.y})].push_back(i);
    }
    std::vector<veylaxis::ReconstructionInput> algorithms;
    for (auto& source : input.areas)
    {
        if (!std::isfinite(source.lower) || !std::isfinite(source.upper) || source.lower > source.upper)
            continue;
        veylaxis::ReconstructionInput reconstruction;
        reconstruction.bands.push_back({source.lower, source.upper});
        std::set<std::size_t> adjacent;
        auto ring = [&](const Ring& r)
        {
            for (std::size_t i = 0; i < r.points.size(); ++i)
            {
                auto found = contourEdges.find(key(r.points[i], r.points[(i + 1) % r.points.size()]));
                if (found != contourEdges.end())
                    for (auto index : found->second)
                    {
                        auto depth = input.contours[index].depth;
                        if (std::abs(depth - source.lower) < 1e-5 || std::abs(depth - source.upper) < 1e-5)
                            adjacent.insert(index);
                    }
            }
        };
        auto polygon = [&](const Polygon& p)
        {
            ring(p.outer);
            for (const auto& h : p.holes)
                ring(h);
        };
        if (auto p = std::get_if<Polygon>(&source.footprint))
            polygon(*p);
        else if (auto m = std::get_if<MultiPolygon>(&source.footprint))
            for (const auto& p : m->polygons)
                polygon(p);
        for (auto index : adjacent)
            reconstruction.contours.push_back(input.contours[index]);
        auto box = geometryBounds(source.footprint);
        auto prepared = std::make_shared<PreparedShape>(source.footprint);
        model->areas.push_back({std::move(source), box, {}, std::move(prepared)});
        algorithms.push_back(std::move(reconstruction));
    }
    for (auto sounding : input.soundings)
        if (std::isfinite(sounding.depth))
        {
            auto area = model->areaAt({sounding.position.x, sounding.position.y});
            if (area)
            {
                const auto& band = model->areas[*area].source;
                if (sounding.depth >= band.lower && sounding.depth <= band.upper)
                {
                    sounding.area = 0;
                    algorithms[*area].soundings.push_back(sounding);
                }
            }
        }
    for (std::size_t i = 0; i < model->areas.size(); ++i)
        model->areas[i].algorithm =
            std::make_shared<veylaxis::LinearContourReconstructor>(std::move(algorithms[i]));
    TerrainSource source;
    source.verticalDatum = input.verticalDatum;
    source.algorithmVersion = "adjacent-contour-soundg-v2-prepared";
    source.elevation = [model](XY p) -> std::optional<double>
    {
        auto area = model->areaAt(p);
        if (!area)
            return {};
        auto estimate = model->areas[*area].algorithm->evaluate({p.x, p.y}, 0);
        if (!estimate)
            return {};
        return -estimate->depth;
    };
    return source;
}
// 从全局采样网格构造端点共享的 Tile 栅格；法线差分也在全局位置取样。
Result<TerrainData> sampleTerrain(const GridSpec& grid, const TileID& tile, double spacing,
                                  const TerrainSource& source)
try
{
    auto count = nodeCount(grid, spacing);
    if (auto e = std::get_if<TileError>(&count))
        return *e;
    if (!source.elevation || source.verticalDatum.empty())
        return TileError{ErrorCode::InvalidRequest, "Invalid terrain source"};
    auto side = std::get<std::uint32_t>(count);
    const auto intervals = side - 1;
    auto bounds = boundsOf(grid, tile);
    if (auto e = std::get_if<TileError>(&bounds))
        return *e;
    auto b = std::get<Bounds>(bounds);
    TerrainData t;
    t.columns = t.rows = side;
    t.northWestSample = {b.minX, b.maxY};
    t.spacingMeters = spacing;
    t.verticalDatum = source.verticalDatum;
    t.provenance = source.provenance;
    const auto size = static_cast<std::size_t>(side) * side;
    t.elevations.assign(size, std::numeric_limits<float>::quiet_NaN());
    t.valid.assign(size, 0);
    t.normals.assign(size, {0, 0, 1});
    double normalStep = std::min(1.0, spacing * .1);
    const auto gx = tile.x * static_cast<std::int64_t>(intervals),
               gy = tile.y * static_cast<std::int64_t>(intervals);
    for (std::uint32_t y = 0; y < side; ++y)
        for (std::uint32_t x = 0; x < side; ++x)
        {
            if (x == 0)
                requestCheckpoint();
            XY p{grid.origin.x + (gx + x) * spacing, grid.origin.y + (gy + intervals - y) * spacing};
            auto z = source.elevation(p);
            auto i = static_cast<std::size_t>(y) * side + x;
            if (!z || !std::isfinite(*z) || std::abs(*z) > std::numeric_limits<float>::max())
                continue;
            t.valid[i] = 1;
            t.elevations[i] = static_cast<float>(*z);
            auto a = source.elevation({p.x - normalStep, p.y}),
                 b2 = source.elevation({p.x + normalStep, p.y}),
                 c = source.elevation({p.x, p.y - normalStep}), d = source.elevation({p.x, p.y + normalStep});
            double dx = a && b2 ? (*b2 - *a) / (2 * normalStep) : 0,
                   dy = c && d ? (*d - *c) / (2 * normalStep) : 0, length = std::sqrt(dx * dx + dy * dy + 1);
            t.normals[i] = {static_cast<float>(-dx / length), static_cast<float>(-dy / length),
                            static_cast<float>(1 / length)};
        }
    return t;
}
catch (const RequestCancelled&)
{
    throw;
}
catch (const std::exception& e)
{
    return TileError{ErrorCode::GenerationFailure, e.what()};
}
}
