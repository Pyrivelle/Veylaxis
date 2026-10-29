// 【阅读导航】真实海底输入：读取 DEPARE 深度区域、DEPCNT 等深线和 SOUNDG 测深点，并统一投影。
#include "BathymetryReader.hpp"
#include "veylaxis/vgds/Bathymetry.hpp"
#include <cmath>
#include <memory>
#include <stdexcept>

namespace veylaxis::vgds
{
namespace
{
Ring ring(const OGRLinearRing& input)
{
    Ring r;
    for (int i = 0; i < input.getNumPoints() - 1; ++i)
        r.points.push_back({input.getX(i), input.getY(i)});
    r.edges.assign(r.points.size(), BoundaryKind::Unknown);
    return r;
}

Polygon polygon(const OGRPolygon& input)
{
    Polygon p;
    p.outer = ring(*input.getExteriorRing());
    for (int i = 0; i < input.getNumInteriorRings(); ++i)
        p.holes.push_back(ring(*input.getInteriorRing(i)));
    return p;
}

void contours(const OGRGeometry& g, double depth, std::vector<veylaxis::Segment>& out)
{
    if (wkbFlatten(g.getGeometryType()) == wkbLineString)
    {
        auto* l = g.toLineString();
        for (int i = 1; i < l->getNumPoints(); ++i)
            out.push_back({{l->getX(i - 1), l->getY(i - 1)}, {l->getX(i), l->getY(i)}, depth});
    }
    else if (OGR_GT_IsSubClassOf(wkbFlatten(g.getGeometryType()), wkbGeometryCollection))
        for (const auto* part : *g.toGeometryCollection())
            contours(*part, depth, out);
}

void soundings(const OGRGeometry& g, std::vector<veylaxis::Sounding>& out)
{
    if (wkbFlatten(g.getGeometryType()) == wkbPoint && g.Is3D())
    {
        auto* p = g.toPoint();
        if (std::isfinite(p->getZ()))
            out.push_back({{p->getX(), p->getY()}, p->getZ(), -1});
    }
    else if (OGR_GT_IsSubClassOf(wkbFlatten(g.getGeometryType()), wkbGeometryCollection))
        for (const auto* part : *g.toGeometryCollection())
            soundings(*part, out);
}
}

TerrainSource readBathymetry(GDALDataset& source, const OGRSpatialReference& target, S57Report& report)
{
    SeabedInput model;
    model.verticalDatum =
        "ENC sounding datum DSPM_SDAT=" + std::to_string(report.soundingDatum) + "; elevation=-depth metres";
    for (const char* name : {"DEPARE", "DEPCNT", "SOUNDG"})
    {
        auto* layer = source.GetLayerByName(name);
        if (!layer)
            continue;
        if (!layer->GetSpatialRef())
            throw std::runtime_error("Depth source missing CRS");
        auto spatial = *layer->GetSpatialRef();
        spatial.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        std::unique_ptr<OGRCoordinateTransformation, decltype(&OCTDestroyCoordinateTransformation)> transform(
            OGRCreateCoordinateTransformation(&spatial, &target), OCTDestroyCoordinateTransformation);
        if (!transform)
            throw std::runtime_error("Depth transform failed");
        layer->ResetReading();
        while (auto* raw = layer->GetNextFeature())
        {
            std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)> f(raw,
                                                                                 OGRFeature::DestroyFeature);
            auto* original = f->GetGeometryRef();
            if (!original || original->IsEmpty())
                continue;
            std::unique_ptr<OGRGeometry> g(original->clone());
            if (g->transform(transform.get()) != OGRERR_NONE)
                throw std::runtime_error("Depth projection failed");
            auto set = [&](const char* field)
            {
                int index = f->GetFieldIndex(field);
                return index >= 0 && f->IsFieldSetAndNotNull(index);
            };
            if (std::string(name) == "DEPARE")
            {
                if (!set("DRVAL1") || !set("DRVAL2"))
                {
                    ++report.rejectedDepthRecords;
                    continue;
                }
                double lower = f->GetFieldAsDouble("DRVAL1"), upper = f->GetFieldAsDouble("DRVAL2");
                if (!std::isfinite(lower) || !std::isfinite(upper) || lower > upper)
                {
                    ++report.rejectedDepthRecords;
                    continue;
                }
                Geometry geometry;
                auto type = wkbFlatten(g->getGeometryType());
                if (type == wkbPolygon)
                    geometry = polygon(*g->toPolygon());
                else if (type == wkbMultiPolygon)
                {
                    MultiPolygon m;
                    for (const auto* p : *g->toMultiPolygon())
                        m.polygons.push_back(polygon(*p));
                    geometry = std::move(m);
                }
                else
                {
                    ++report.rejectedDepthRecords;
                    continue;
                }
                model.areas.push_back({report.cell + ":DEPARE:" + std::to_string(f->GetFID()),
                                       std::move(geometry), lower, upper});
                ++report.depthAreas;
            }
            else if (std::string(name) == "DEPCNT")
            {
                if (!set("VALDCO"))
                {
                    ++report.rejectedDepthRecords;
                    continue;
                }
                double depth = f->GetFieldAsDouble("VALDCO");
                if (!std::isfinite(depth))
                {
                    ++report.rejectedDepthRecords;
                    continue;
                }
                contours(*g, depth, model.contours);
                ++report.depthContours;
            }
            else
                soundings(*g, model.soundings);
        }
    }
    report.soundings = model.soundings.size();
    return makeSeabedSource(std::move(model));
}
}
