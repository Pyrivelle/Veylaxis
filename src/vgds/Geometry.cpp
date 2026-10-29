// 【阅读导航】GDAL/GEOS 几何适配：处理带孔、多面、多段裁剪，并区分原始边与人工切块边。
#include "veylaxis/vgds/Service.hpp"
#include <ogrsf_frmts.h>
#include <algorithm>
#include <cmath>
#include <memory>
#include <stdexcept>

namespace veylaxis::vgds
{
namespace
{
using G = std::unique_ptr<OGRGeometry>;

OGRLinearRing ogrRing(const Ring& ring)
{
    if (ring.points.size() < 3 || ring.edges.size() != ring.points.size())
        throw std::invalid_argument("Invalid source ring/edge count");
    for (auto p : ring.points)
        if (!std::isfinite(p.x) || !std::isfinite(p.y))
            throw std::invalid_argument("Nonfinite source coordinate");
    OGRLinearRing r;
    for (auto p : ring.points)
        r.addPoint(p.x, p.y);
    r.closeRings();
    return r;
}

std::unique_ptr<OGRPolygon> ogrPolygon(const Polygon& polygon)
{
    auto p = std::make_unique<OGRPolygon>();
    auto r = ogrRing(polygon.outer);
    p->addRing(&r);
    for (const auto& h : polygon.holes)
    {
        r = ogrRing(h);
        p->addRing(&r);
    }
    return p;
}

G ogr(const Geometry& g)
{
    if (auto p = std::get_if<Polygon>(&g))
        return ogrPolygon(*p);
    if (auto m = std::get_if<MultiPolygon>(&g))
    {
        auto out = std::make_unique<OGRMultiPolygon>();
        for (const auto& p : m->polygons)
            out->addGeometryDirectly(ogrPolygon(p).release());
        return out;
    }
    if (auto l = std::get_if<LineString>(&g))
    {
        if (l->points.size() < 2 || l->edges.size() != l->points.size() - 1)
            throw std::invalid_argument("Invalid source line");
        auto out = std::make_unique<OGRLineString>();
        for (auto p : l->points)
            out->addPoint(p.x, p.y);
        return out;
    }
    auto p = std::get<Point>(g).position;
    return std::make_unique<OGRPoint>(p.x, p.y);
}

struct Edge
{
    XY a, b;
    BoundaryKind kind;
};

std::vector<Edge> sourceEdges(const Geometry& g)
{
    std::vector<Edge> out;
    auto add = [&](const auto& r, bool closed)
    {
        for (std::size_t i = 0; i < r.edges.size(); ++i)
            out.push_back({r.points[i], r.points[closed ? (i + 1) % r.points.size() : i + 1], r.edges[i]});
    };
    auto polygon = [&](const Polygon& p)
    {
        add(p.outer, true);
        for (const auto& h : p.holes)
            add(h, true);
    };
    if (auto p = std::get_if<Polygon>(&g))
        polygon(*p);
    else if (auto m = std::get_if<MultiPolygon>(&g))
        for (const auto& part : m->polygons)
            polygon(part);
    else if (auto l = std::get_if<LineString>(&g))
        add(*l, false);
    return out;
}

bool onSegment(XY p, const Edge& e)
{
    double dx = e.b.x - e.a.x, dy = e.b.y - e.a.y, length = std::hypot(dx, dy);
    if (length == 0)
        return false;
    const double t = ((p.x - e.a.x) * dx + (p.y - e.a.y) * dy) / (length * length);
    return t >= -1e-9 && t <= 1 + 1e-9 && std::abs((p.x - e.a.x) * dy - (p.y - e.a.y) * dx) / length <= 1e-7;
}

BoundaryKind role(XY a, XY b, const std::vector<Edge>& edges)
{
    for (const auto& e : edges)
        if (onSegment(a, e) && onSegment(b, e))
            return e.kind;
    return BoundaryKind::ArtificialClip;
}

Ring ring(const OGRLinearRing& input, const std::vector<Edge>& edges, const Bounds& b)
{
    Ring r;
    // Snap only computed rectangle intersections, avoiding sub-micrometre drift.
    auto snap = [](double v, double min, double max)
    {
        if (std::abs(v - min) < 1e-8)
            return min;
        if (std::abs(v - max) < 1e-8)
            return max;
        return v;
    };
    for (int i = 0; i < input.getNumPoints() - 1; ++i)
        r.points.push_back({snap(input.getX(i), b.minX, b.maxX), snap(input.getY(i), b.minY, b.maxY)});
    for (std::size_t i = 0; i < r.points.size(); ++i)
        r.edges.push_back(role(r.points[i], r.points[(i + 1) % r.points.size()], edges));
    return r;
}

void polygons(const OGRGeometry& g, MultiPolygon& out, const std::vector<Edge>& edges, const Bounds& b)
{
    if (wkbFlatten(g.getGeometryType()) == wkbPolygon)
    {
        const auto& p = *g.toPolygon();
        if (p.get_Area() <= 1e-10)
            return;
        Polygon v;
        v.outer = ring(*p.getExteriorRing(), edges, b);
        for (int i = 0; i < p.getNumInteriorRings(); ++i)
            v.holes.push_back(ring(*p.getInteriorRing(i), edges, b));
        out.polygons.push_back(std::move(v));
    }
    else if (OGR_GT_IsSubClassOf(wkbFlatten(g.getGeometryType()), wkbGeometryCollection))
    {
        for (const auto* part : *g.toGeometryCollection())
            polygons(*part, out, edges, b);
    }
}

void lines(const OGRGeometry& g, std::vector<LineString>& out, const std::vector<Edge>& edges)
{
    if (wkbFlatten(g.getGeometryType()) == wkbLineString)
    {
        auto* l = g.toLineString();
        if (l->getNumPoints() < 2 || l->get_Length() <= 1e-10)
            return;
        LineString v;
        for (int i = 0; i < l->getNumPoints(); ++i)
            v.points.push_back({l->getX(i), l->getY(i)});
        for (std::size_t i = 1; i < v.points.size(); ++i)
            v.edges.push_back(role(v.points[i - 1], v.points[i], edges));
        out.push_back(std::move(v));
    }
    else if (OGR_GT_IsSubClassOf(wkbFlatten(g.getGeometryType()), wkbGeometryCollection))
        for (const auto* part : *g.toGeometryCollection())
            lines(*part, out, edges);
}
}

double polygonArea(const Geometry& geometry)
{
    auto g = ogr(geometry);
    return OGR_G_Area(OGRGeometry::ToHandle(g.get()));
}
// 裁剪几何的同时保留原物标身份和属性；新边需要与源边比对，才能判定真实边界。
Result<std::vector<Feature>> intersectFeature(const Feature& f, const Geometry& mask, const Bounds& b,
                                              const TileID& tile)
try
{
    requestCheckpoint();
    auto source = ogr(f.geometry);
    if (!source->IsValid())
        throw std::runtime_error("Invalid source geometry: " + f.sourceObjectId);
    auto rectangle = ogr(mask);
    G clipped(source->Intersection(rectangle.get()));
    if (!clipped)
        throw std::runtime_error("GEOS intersection failed");
    if (clipped->IsEmpty())
        return std::vector<Feature>{};
    G normalized(clipped->Normalize());
    if (!normalized)
        throw std::runtime_error("GEOS normalization failed");
    const auto edges = sourceEdges(f.geometry);
    std::vector<Feature> out;
    auto emit = [&](Geometry g)
    {
        auto v = f;
        v.geometry = std::move(g);
        v.fragmentId = f.sourceObjectId + "@" + tile.gridId + ":" + std::to_string(tile.x) + "," +
                       std::to_string(tile.y) + ":" + std::to_string(out.size());
        out.push_back(std::move(v));
    };
    if (std::holds_alternative<Polygon>(f.geometry) || std::holds_alternative<MultiPolygon>(f.geometry))
    {
        MultiPolygon result;
        polygons(*normalized, result, edges, b);
        if (result.polygons.size() == 1)
            emit(result.polygons.front());
        else if (!result.polygons.empty())
            emit(result);
    }
    else if (std::holds_alternative<LineString>(f.geometry))
    {
        std::vector<LineString> result;
        lines(*normalized, result, edges);
        for (auto& l : result)
            emit(std::move(l));
    }
    else
    {
        auto p = std::get<Point>(f.geometry);
        if (p.position.x >= b.minX && p.position.x < b.maxX && p.position.y >= b.minY &&
            p.position.y < b.maxY)
            emit(p);
    }
    return out;
}
catch (const RequestCancelled&)
{
    throw;
}
catch (const std::exception& e)
{
    return TileError{ErrorCode::GenerationFailure, e.what()};
}

Result<std::vector<Feature>> clipFeature(const Feature& f, const Bounds& b, const TileID& tile)
{
    return intersectFeature(
        f,
        Polygon{Ring{{{b.minX, b.minY}, {b.maxX, b.minY}, {b.maxX, b.maxY}, {b.minX, b.maxY}},
                     std::vector<BoundaryKind>(4, BoundaryKind::ArtificialClip)},
                {}},
        b, tile);
}

Bounds geometryBounds(const Geometry& geometry)
{
    auto g = ogr(geometry);
    OGREnvelope e;
    g->getEnvelope(&e);
    return {e.MinX, e.MinY, e.MaxX, e.MaxY};
}
}
