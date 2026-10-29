// 【阅读导航】可替换的海底估算算法：相邻等深线线性插值，加测深点残差约束；深度正向下。
#include "veylaxis/reconstruction.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace veylaxis
{
double segmentDistance(XY p, const Segment& s)
{
    const double dx = s.b.x - s.a.x, dy = s.b.y - s.a.y;
    const double len = dx * dx + dy * dy;
    const double t = len > 0 ? std::clamp(((p.x - s.a.x) * dx + (p.y - s.a.y) * dy) / len, 0.0, 1.0) : 0;
    return std::hypot(p.x - s.a.x - t * dx, p.y - s.a.y - t * dy);
}

LinearContourReconstructor::LinearContourReconstructor(ReconstructionInput input) : input_(std::move(input))
{
    if (!(input_.soundingRadius > 0) || !std::isfinite(input_.soundingRadius))
        throw std::runtime_error("Invalid sounding radius");
    for (auto b : input_.bands)
        if (!std::isfinite(b.lower) || !std::isfinite(b.upper) || b.lower > b.upper)
            throw std::runtime_error("Invalid depth band");
    for (auto& s : input_.soundings)
    {
        auto b = base(s.position, s.area);
        residuals_.push_back(b && b->taper > 1e-8 ? (s.depth - b->depth) / b->taper : 0);
    }
}

std::optional<LinearContourReconstructor::Base> LinearContourReconstructor::base(XY p, int area) const
{
    if (area < 0 || area >= static_cast<int>(input_.bands.size()))
        return std::nullopt;
    const auto band = input_.bands[area];
    double dl = std::numeric_limits<double>::infinity(), du = dl;
    auto visit = [&](const Segment& s)
    {
        if (std::abs(s.depth - band.lower) < 1e-5)
            dl = std::min(dl, segmentDistance(p, s));
        if (std::abs(s.depth - band.upper) < 1e-5)
            du = std::min(du, segmentDistance(p, s));
    };
    if (input_.areaContourIndices.empty())
    {
        for (const auto& s : input_.contours)
            visit(s);
    }
    else
    {
        for (size_t index : input_.areaContourIndices.at(area))
            visit(input_.contours.at(index));
    }
    if (!std::isfinite(dl) && !std::isfinite(du))
        return std::nullopt;
    if (!std::isfinite(dl) || !std::isfinite(du))
    {
        double distance = std::min(dl, du);
        double t = std::clamp(distance / input_.soundingRadius, 0.0, 1.0);
        return Base{std::isfinite(dl) ? band.lower : band.upper, t * t * (3 - 2 * t), 1};
    }
    double t = dl + du > 1e-10 ? dl / (dl + du) : 0.5;
    return Base{band.lower + (band.upper - band.lower) * t, 4 * t * (1 - t), 0};
}

// 核心估值步骤：等深线坡面给出基础深度，再以附近测深点残差修正。
std::optional<Estimate> LinearContourReconstructor::evaluate(XY p, int area) const
{
    auto b = base(p, area);
    if (!b)
        return std::nullopt;
    double numerator = 0, denominator = 1;
    const auto band = input_.bands[area];
    for (size_t i = 0; i < input_.soundings.size(); ++i)
    {
        const auto& s = input_.soundings[i];
        if (s.area != area)
            continue;
        const double d = std::hypot(p.x - s.position.x, p.y - s.position.y);
        if (d >= input_.soundingRadius)
            continue;
        if (d < 1e-6 && b->taper > 1e-8)
            return Estimate{std::clamp(s.depth, band.lower, band.upper), b->quality};
        const double r = d / input_.soundingRadius;
        const double w = std::pow(1 - r, 4) * (4 * r + 1) / std::max(r * r, 1e-16);
        numerator += w * residuals_[i];
        denominator += w;
    }
    return Estimate{std::clamp(b->depth + b->taper * numerator / denominator, band.lower, band.upper),
                    b->quality};
}

int repairDepthAreaConflicts(std::vector<float>& elevations, std::vector<unsigned char>& quality, int width,
                             int height, XY origin, double cellSize, const std::vector<Segment>& conflicts,
                             double radius, double coreRadius)
{
    if (conflicts.empty())
        return 0;
    if (!(radius > coreRadius && coreRadius > 0 && cellSize > 0))
        throw std::runtime_error("Invalid conflict repair radii");
    const auto raw = elevations;
    int changed = 0;
    const int cells = static_cast<int>(std::ceil(radius / cellSize));
    for (int r = 0; r < height; ++r)
        for (int c = 0; c < width; ++c)
        {
            const size_t i = static_cast<size_t>(r) * width + c;
            if (!quality[i])
                continue;
            XY p{origin.x + (c + 0.5) * cellSize, origin.y - (r + 0.5) * cellSize};
            double distance = std::numeric_limits<double>::infinity();
            for (const auto& s : conflicts)
                distance = std::min(distance, segmentDistance(p, s));
            if (distance >= radius)
                continue;
            double sum = 0, weights = 0;
            for (int rr = std::max(0, r - cells); rr <= std::min(height - 1, r + cells); ++rr)
                for (int cc = std::max(0, c - cells); cc <= std::min(width - 1, c + cells); ++cc)
                {
                    const size_t j = static_cast<size_t>(rr) * width + cc;
                    if (!quality[j])
                        continue;
                    const double t = std::hypot(static_cast<double>(rr - r), static_cast<double>(cc - c)) *
                                     cellSize / radius;
                    if (t >= 1)
                        continue;
                    const double w = std::pow(1 - t, 4) * (4 * t + 1);
                    sum += w * raw[j];
                    weights += w;
                }
            double t = std::clamp((distance - coreRadius) / (radius - coreRadius), 0.0, 1.0);
            const double blend = 1 - t * t * (3 - 2 * t);
            elevations[i] = static_cast<float>((1 - blend) * raw[i] + blend * sum / weights);
            quality[i] = 3;
            ++changed;
        }
    return changed;
}
}
