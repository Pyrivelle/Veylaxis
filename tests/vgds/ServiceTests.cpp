#include "veylaxis/vgds/Service.hpp"
#include "veylaxis/vgds/Codec.hpp"
#include "veylaxis/vgds/Grid.hpp"
#ifdef VEYLAXIS_HAS_OSGB
#include "veylaxis/vgds/OsgBuilder.hpp"
#include <osg/Geode>
#include <osg/Geometry>
#include <osg/ValueObject>
#include <osgDB/ReadFile>
#endif
#include <filesystem>
#include <chrono>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <cmath>

namespace
{
using namespace veylaxis::vgds;
int checks;

void check(bool b, const char* m)
{
    ++checks;
    if (!b)
        throw std::runtime_error(m);
}

template <class T> T value(Result<T> r)
{
    if (auto e = std::get_if<TileError>(&r))
        throw std::runtime_error(e->message);
    return std::get<T>(std::move(r));
}

Ring ring(std::vector<XY> p)
{
    auto n = p.size();
    return {std::move(p), std::vector<BoundaryKind>(n, BoundaryKind::Geographic)};
}

void rings(const Geometry& g, const std::function<void(const Ring&)>& fn)
{
    auto add = [&](const Polygon& p)
    {
        fn(p.outer);
        for (const auto& h : p.holes)
            fn(h);
    };
    if (auto p = std::get_if<Polygon>(&g))
        add(*p);
    else if (auto m = std::get_if<MultiPolygon>(&g))
        for (const auto& part : m->polygons)
            add(part);
}

std::set<std::pair<double, double>> seam(const TilePackage& p, double x)
{
    std::set<std::pair<double, double>> s;
    for (const auto& f : p.features)
        rings(f.geometry,
              [&](const Ring& r)
              {
                  for (std::size_t i = 0; i < r.points.size(); ++i)
                      if (r.edges[i] == BoundaryKind::ArtificialClip)
                      {
                          auto a = r.points[i], b = r.points[(i + 1) % r.points.size()];
                          if (a.x == x && b.x == x)
                              s.emplace(std::min(a.y, b.y), std::max(a.y, b.y));
                      }
              });
    return s;
}
}
int runVgdsServiceTests()
try
{
    checks = 0;
    GridSpec grid{"s2-test", "EPSG:32619", {503500, 4856500}, 1000};
    auto d = islandFixture(grid);
    check(std::get<Polygon>(d.features[0].geometry).outer.points.size() == 80, "80 point fixture");
    check(std::get<Polygon>(d.features[0].geometry).holes.size() == 2, "Fixture holes");
    check(contentHash("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "SHA256 known vector");
    // Unique test root: never erase a user cache tree or share parallel runs.
    auto root = std::filesystem::temp_directory_path() /
                ("veylaxis-s2-" +
                 contentHash(std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))
                     .substr(0, 16));
    ServiceConfig c;
    c.cacheDirectory = root.string();
    LocalService service(d, c);
    double total = 0;
    int holes = 0, artificial = 0;
    TilePackage left, right;
    for (int y = -1; y <= 1; ++y)
        for (int x = 0; x <= 1; ++x)
        {
            TileRequest r{"test", {grid.id, x, y}, {"land"}};
            auto p = value(service.request(r));
            check(!validate(p), "Valid clipped package");
            for (const auto& f : p.features)
            {
                total += polygonArea(f.geometry);
                check(f.sourceObjectId == d.features[0].sourceObjectId, "Source identity");
                rings(f.geometry,
                      [&](const Ring& ring)
                      {
                          for (auto e : ring.edges)
                              if (e == BoundaryKind::ArtificialClip)
                                  ++artificial;
                      });
                if (auto poly = std::get_if<Polygon>(&f.geometry))
                    holes += static_cast<int>(poly->holes.size());
            }
            if (y == 0)
            {
                if (x == 0)
                    left = p;
                else
                    right = p;
            }
        }
    check(std::abs(total - polygonArea(d.features[0].geometry)) < 1e-5,
          "Area conservation across six tiles including holes");
    check(holes == 2, "Contained holes preserved");
    check(artificial > 0, "Cut edges tagged");
    check(!left.features.empty() && !right.features.empty(), "Cross-tile island");
    check(left.features[0].fragmentId != right.features[0].fragmentId, "Distinct fragment identities");
    auto a = seam(left, grid.origin.x + 1000), b = seam(right, grid.origin.x + 1000);
    check(!a.empty() && a == b, "Identical public boundary intervals");
    TileRequest request{"new-correlation", {grid.id, 0, 0}, {"land"}};
    auto hits = service.stats().hits;
    auto cached = value(service.request(request));
    check(service.stats().hits == hits + 1, "Repeat hits cache");
    check(cached.metadata.requestId == "new-correlation", "Fresh request ID on hit");
    LocalService restarted(d, c);
    check(value(restarted.request(request)).features.size() == cached.features.size() &&
              restarted.stats().hits == 1,
          "Persistent cache restart");
    auto encoded = value(service.requestJson(value(encodeRequestJson(request))));
    check(!validate(value(decodeJson(encoded))), "Local serialized request response");
    check(std::holds_alternative<TileError>(service.requestJson("{")), "Malformed local request");
    check(value(service.request({"empty", {grid.id, 10, 10}, {"land", "navigation_aids"}}))
                  .metadata.layers.at("land") == LayerState::Empty,
          "Empty tile success");
    // A U polygon intersected above its connector produces two components.
    Feature u = d.features[0];
    u.sourceObjectId = "mock:u";
    u.geometry = Polygon{
        ring({{0, 0}, {600, 0}, {600, 900}, {400, 900}, {400, 200}, {200, 200}, {200, 900}, {0, 900}}), {}};
    auto split = value(clipFeature(u, {0, 300, 600, 800}, {grid.id, 0, 0}));
    check(split.size() == 1 && std::holds_alternative<MultiPolygon>(split[0].geometry),
          "Disconnected output retains MultiPolygon");
    check(std::get<MultiPolygon>(split[0].geometry).polygons.size() == 2, "Two connected components");
    check(std::abs(polygonArea(split[0].geometry) - 200000) < 1e-6, "Disconnected area");
    // A hole cut by the seam becomes two open notches, never a filled lake.
    Feature lake = u;
    lake.geometry = Polygon{ring({{0, 0}, {1000, 0}, {1000, 1000}, {0, 1000}}),
                            {ring({{400, 200}, {600, 200}, {600, 800}, {400, 800}})}};
    auto l = value(clipFeature(lake, {0, 0, 500, 1000}, {grid.id, 0, 0})),
         r = value(clipFeature(lake, {500, 0, 1000, 1000}, {grid.id, 1, 0}));
    check(std::abs(polygonArea(l[0].geometry) + polygonArea(r[0].geometry) - 880000) < 1e-6,
          "Seam-crossing hole area");
    auto changed = d;
    changed.features[0].properties["revision"] = std::int64_t(2);
    LocalService revised(changed, c);
    value(revised.request(request));
    check(revised.stats().generations == 1, "Input property change invalidates cache");
    auto changedGrid = d;
    changedGrid.grid.origin.x += 100;
    LocalService shifted(changedGrid, c);
    value(shifted.request(request));
    check(shifted.stats().generations == 1, "Grid change invalidates cache");
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root))
        if (entry.path().filename() == "tile.json")
        {
            std::ofstream out(entry.path());
            out << "corrupt";
        }
    value(service.request(request));
    check(service.stats().repairs == 1, "Corrupt package regeneration");
#ifdef VEYLAXIS_HAS_OSGB
    ServiceConfig rendered = c;
    rendered.osgbEnabled = true;
    rendered.builder = buildOsgb;
    rendered.builderVersion = OsgBuilderVersion;
    LocalService withRender(d, rendered);
    auto lp = value(withRender.request(request));
    auto rp = value(withRender.request({"right", {grid.id, 1, 0}, {"land"}}));
    check(lp.artifacts.size() == 1 && lp.features.size() > 0, "OSGB plus original data");
    check(value(decodeJson(value(encodeJson(lp)))).artifacts[0].contentHash == lp.artifacts[0].contentHash,
          "Artifact metadata roundtrip");
    check(lp.artifacts[0].zScale == 1, "OSGB Z scale 1");
    std::set<std::pair<double, double>> meshEdges[2];
    for (int side = 0; side < 2; ++side)
    {
        const auto& p = side == 0 ? lp : rp;
        auto model = osgDB::readRefNodeFile(p.artifacts[0].path);
        check(model.valid(), "Read OSGB");

        struct Visitor : osg::NodeVisitor
        {
            double originX, originY, seamX;
            std::set<std::pair<double, double>>& shared;
            int& checks;

            Visitor(double x, double y, double cut, std::set<std::pair<double, double>>& s, int& c)
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN), originX(x), originY(y), seamX(cut), shared(s),
                  checks(c)
            {
            }

            void apply(osg::Geode& node) override
            {
                for (unsigned j = 0; j < node.getNumDrawables(); ++j)
                {
                    auto* geometry = node.getDrawable(j)->asGeometry();
                    if (!geometry)
                        continue;
                    auto* v = dynamic_cast<osg::Vec3dArray*>(geometry->getVertexArray());
                    check(v != nullptr, "Double local coordinates");
                    if (geometry->getName() == "estimated-flat-top")
                        for (auto p : *v)
                        {
                            check(p.z() == 12, "Globally identical flat estimated height");
                            if (std::abs(p.x() + originX - seamX) < 1e-8)
                                shared.emplace(p.y() + originY, p.z());
                        }
                    else if (geometry->getName() == "geographic-walls-only")
                        for (std::size_t i = 0; i + 3 < v->size(); i += 4)
                            check(!((*v)[i].x() + originX == seamX && (*v)[i + 1].x() + originX == seamX),
                                  "No wall at artificial seam");
                }
            }
        } visitor(p.metadata.tileOrigin.x, p.metadata.tileOrigin.y, grid.origin.x + 1000, meshEdges[side],
                  checks);

        model->accept(visitor);
    }
    check(!meshEdges[0].empty() && meshEdges[0] == meshEdges[1], "OSGB world-space seam XYZ continuity");
    {
        std::ofstream out(lp.artifacts[0].path);
        out << "broken osgb";
    }
    value(withRender.request(request));
    check(withRender.stats().repairs == 1, "Corrupt OSGB rebuild");
    check(value(service.request(request)).artifacts.empty(), "Runtime OSGB disable");
#endif
    std::cout << "VGDS S2: " << checks << " checks passed; area=" << total
              << " m2; shared intervals=" << a.size() << "\n";
    return 0;
}
catch (const std::exception& e)
{
    std::cerr << "VGDS S2 failed: " << e.what() << '\n';
    return 1;
}
