// 【阅读导航】模拟岛屿统一高度场：保留完整源岛轮廓，用真实海岸线距离生成起伏，不从裁剪边生成山坡。
#include "veylaxis/vgds/IslandTerrain.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <numeric>
#include <stdexcept>
#include <nlohmann/json.hpp>

namespace veylaxis::vgds
{
namespace
{
struct Segment
{
    XY a, b;
};

double distance2(XY p, const Segment& s)
{
    double x = s.b.x - s.a.x, y = s.b.y - s.a.y, l = x * x + y * y,
           t = l ? std::clamp(((p.x - s.a.x) * x + (p.y - s.a.y) * y) / l, 0.0, 1.0) : 0;
    double dx = p.x - s.a.x - t * x, dy = p.y - s.a.y - t * y;
    return dx * dx + dy * dy;
}

struct Coast
{
    std::vector<Segment> segments;

    struct Node
    {
        Bounds b;
        std::size_t first{}, count{};
        int left{-1}, right{-1};
    };

    std::vector<Node> nodes;

    int build(std::size_t first, std::size_t count)
    {
        Node node;
        node.b = {INFINITY, INFINITY, -INFINITY, -INFINITY};
        node.first = first;
        node.count = count;
        for (auto i = first; i < first + count; ++i)
            for (auto p : {segments[i].a, segments[i].b})
            {
                node.b.minX = std::min(node.b.minX, p.x);
                node.b.minY = std::min(node.b.minY, p.y);
                node.b.maxX = std::max(node.b.maxX, p.x);
                node.b.maxY = std::max(node.b.maxY, p.y);
            }
        const int index = static_cast<int>(nodes.size());
        nodes.push_back(node);
        if (count <= 12)
            return index;
        bool x = node.b.maxX - node.b.minX > node.b.maxY - node.b.minY;
        auto mid = first + count / 2;
        std::nth_element(segments.begin() + first, segments.begin() + mid, segments.begin() + first + count,
                         [&](const auto& a, const auto& b)
                         {
                             return x ? a.a.x + a.b.x < b.a.x + b.b.x : a.a.y + a.b.y < b.a.y + b.b.y;
                         });
        nodes[index].left = build(first, mid - first);
        nodes[index].right = build(mid, first + count - mid);
        return index;
    }

    static double lower(XY p, const Bounds& b)
    {
        auto dx = std::max({b.minX - p.x, 0.0, p.x - b.maxX}),
             dy = std::max({b.minY - p.y, 0.0, p.y - b.maxY});
        return dx * dx + dy * dy;
    }

    void nearest(int index, XY p, double& best) const
    {
        const auto& n = nodes[index];
        if (lower(p, n.b) > best)
            return;
        if (n.left < 0)
        {
            for (auto j = n.first; j < n.first + n.count; ++j)
                best = std::min(best, distance2(p, segments[j]));
            return;
        }
        int a = n.left, b = n.right;
        if (lower(p, nodes[a].b) > lower(p, nodes[b].b))
            std::swap(a, b);
        nearest(a, p, best);
        nearest(b, p, best);
    }

    double distance(XY p) const
    {
        if (nodes.empty())
            return 0;
        double best = INFINITY;
        nearest(0, p, best);
        return std::sqrt(best);
    }
};

std::shared_ptr<Coast> coast(const Geometry& g)
{
    auto result = std::make_shared<Coast>();
    auto ring = [&](const Ring& r)
    {
        for (std::size_t i = 0; i < r.points.size(); ++i)
            if (r.edges.at(i) == BoundaryKind::Geographic)
                result->segments.push_back({r.points[i], r.points[(i + 1) % r.points.size()]});
    };
    auto polygon = [&](const Polygon& p)
    {
        ring(p.outer);
        for (const auto& h : p.holes)
            ring(h);
    };
    if (auto p = std::get_if<Polygon>(&g))
        polygon(*p);
    else if (auto m = std::get_if<MultiPolygon>(&g))
        for (const auto& p : m->polygons)
            polygon(p);
    if (!result->segments.empty())
        result->build(0, result->segments.size());
    return result;
}
}

struct IslandTerrain::Impl
{
    IslandTerrainConfig config;
    mutable std::mutex mutex;
    std::uint64_t clock{};

    struct Source
    {
        std::string version;
        Geometry geometry;
        mutable std::shared_ptr<Coast> coast;
    };

    struct Cell
    {
        std::string version;
        std::uint64_t touched;
        std::map<std::string, Source> objects;
    };

    std::map<std::string, Cell> cells;
};

IslandTerrain::IslandTerrain(IslandTerrainConfig config) : impl_(std::make_unique<Impl>())
{
    if (!std::isfinite(config.heightMeters) || config.heightMeters <= 0 ||
        !std::isfinite(config.shoreScaleMeters) || config.shoreScaleMeters <= 0 ||
        !std::isfinite(config.slopeExponent) || config.slopeExponent < 1 ||
        !std::isfinite(config.sampleSpacingMeters) || config.sampleSpacingMeters <= 0 ||
        !std::isfinite(config.normalStepMeters) || config.normalStepMeters <= 0 ||
        !std::isfinite(config.baseDepthMeters) || config.baseDepthMeters < 0 || config.maxSourceCells < 1 ||
        config.maxSourceCells > 64)
        throw std::invalid_argument("Invalid procedural island configuration");
    impl_->config = config;
}

IslandTerrain::~IslandTerrain() = default;

const IslandTerrainConfig& IslandTerrain::config() const
{
    return impl_->config;
}

std::string IslandTerrain::version() const
{
    const auto& c = impl_->config;
    return "coast-field-v1:" +
           contentHash(nlohmann::json::array({c.heightMeters, c.shoreScaleMeters, c.slopeExponent,
                                              c.sampleSpacingMeters, c.normalStepMeters, c.baseDepthMeters})
                           .dump());
}

// 高度场保存完整源岛轮廓，而不是当前 Tile 裁下来的多边形。
void IslandTerrain::addDataset(const Dataset& dataset)
{
    std::lock_guard<std::mutex> guard(impl_->mutex);
    auto& cell = impl_->cells[dataset.id];
    cell.touched = ++impl_->clock;
    if (cell.version == dataset.version)
        return;
    cell.version = dataset.version;
    cell.objects.clear();
    for (const auto& f : dataset.features)
        if (f.layer == "land" &&
            (std::holds_alternative<Polygon>(f.geometry) || std::holds_alternative<MultiPolygon>(f.geometry)))
            cell.objects.emplace(f.sourceObjectId, Impl::Source{f.dataVersion, f.geometry, {}});
    while (impl_->cells.size() > impl_->config.maxSourceCells)
    {
        auto oldest = std::min_element(impl_->cells.begin(), impl_->cells.end(),
                                       [](const auto& a, const auto& b)
                                       {
                                           return a.second.touched < b.second.touched;
                                       });
        impl_->cells.erase(oldest);
    }
}

std::size_t IslandTerrain::sourceCellCount() const
{
    std::lock_guard<std::mutex> guard(impl_->mutex);
    return impl_->cells.size();
}

// 同一全局点使用同一海岸距离函数；法线用同一函数的差分，避免块边高度/光照接缝。
std::optional<IslandSample> IslandTerrain::sample(const std::string& id, const std::string& version,
                                                  XY p) const
{
    if (!std::isfinite(p.x) || !std::isfinite(p.y))
        return {};
    std::shared_ptr<Coast> source;
    {
        std::lock_guard<std::mutex> guard(impl_->mutex);
        for (const auto& c : impl_->cells)
        {
            auto it = c.second.objects.find(id);
            if (it != c.second.objects.end() && it->second.version == version)
            {
                if (!it->second.coast)
                    it->second.coast = coast(it->second.geometry);
                source = it->second.coast;
                break;
            }
        }
    }
    if (!source)
        return {};
    const auto& c = impl_->config;
    auto height = [&](XY q)
    {
        return c.heightMeters *
               std::pow(std::clamp(source->distance(q) / c.shoreScaleMeters, 0.0, 1.0), c.slopeExponent);
    };
    IslandSample sample;
    sample.elevation = height(p);
    double dx = (height({p.x + c.normalStepMeters, p.y}) - height({p.x - c.normalStepMeters, p.y})) /
                (2 * c.normalStepMeters),
           dy = (height({p.x, p.y + c.normalStepMeters}) - height({p.x, p.y - c.normalStepMeters})) /
                (2 * c.normalStepMeters);
    double length = std::sqrt(dx * dx + dy * dy + 1);
    sample.normal = {-dx / length, -dy / length, 1 / length};
    return sample;
}
}
