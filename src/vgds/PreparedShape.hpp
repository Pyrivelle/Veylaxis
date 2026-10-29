#pragma once
#include "veylaxis/vgds/Types.hpp"
#include <ogrsf_frmts.h>
#include <memory>
#include <stdexcept>

namespace veylaxis::vgds
{
class PreparedShape
{
    std::unique_ptr<OGRGeometry> geometry_;
    OGRPreparedGeometryH prepared_{};

    static std::unique_ptr<OGRPolygon> polygon(const Polygon& input)
    {
        auto output = std::make_unique<OGRPolygon>();
        auto add = [&](const Ring& r)
        {
            OGRLinearRing ring;
            for (auto p : r.points)
                ring.addPoint(p.x, p.y);
            ring.closeRings();
            output->addRing(&ring);
        };
        add(input.outer);
        for (const auto& hole : input.holes)
            add(hole);
        return output;
    }

  public:
    explicit PreparedShape(const OGRGeometry& geometry) : geometry_(geometry.clone())
    {
        prepared_ = OGRCreatePreparedGeometry(OGRGeometry::ToHandle(geometry_.get()));
    }

    explicit PreparedShape(const Geometry& input)
    {
        if (auto p = std::get_if<Polygon>(&input))
            geometry_ = polygon(*p);
        else if (auto m = std::get_if<MultiPolygon>(&input))
        {
            auto multi = std::make_unique<OGRMultiPolygon>();
            for (const auto& p : m->polygons)
                multi->addGeometryDirectly(polygon(p).release());
            geometry_ = std::move(multi);
        }
        else
            throw std::invalid_argument("Prepared area must be polygonal");
        prepared_ = OGRCreatePreparedGeometry(OGRGeometry::ToHandle(geometry_.get()));
    }

    ~PreparedShape()
    {
        if (prepared_)
            OGRDestroyPreparedGeometry(prepared_);
    }

    bool intersects(XY p) const
    {
        OGRPoint point(p.x, p.y);
        return prepared_ ? OGRPreparedGeometryIntersects(prepared_, OGRGeometry::ToHandle(&point)) != 0
                         : geometry_->Intersects(&point) != 0;
    }
};
}
