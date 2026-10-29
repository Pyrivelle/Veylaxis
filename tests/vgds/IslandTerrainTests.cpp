#include "veylaxis/vgds/IslandTerrain.hpp"
#include "veylaxis/vgds/Grid.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>
#ifdef VEYLAXIS_HAS_OSGB
#include "veylaxis/vgds/OsgBuilder.hpp"
#include <osgDB/ReadFile>
#include <osg/Geode>
#include <osg/Geometry>
#include <map>
#include <filesystem>
#include <chrono>
#endif
namespace
{
using namespace veylaxis::vgds;
int checks;

void check(bool v, const char* m)
{
    ++checks;
    if (!v)
        throw std::runtime_error(m);
}

template <class T> T value(Result<T> r)
{
    if (auto e = std::get_if<TileError>(&r))
        throw std::runtime_error(e->message);
    return std::get<T>(std::move(r));
}
}
int runIslandTerrainTests()
try
{
    checks = 0;
    GridSpec grid{"island-test", "EPSG:32619", {503500, 4856500}, 1000};
    auto data = islandFixture(grid);
    IslandTerrainConfig config;
    config.enabled = true;
    auto field = std::make_shared<IslandTerrain>(config);
    field->addDataset(data);
    auto& f = data.features[0];
    auto original = std::get<Polygon>(f.geometry);
    auto coast = field->sample(f.sourceObjectId, f.dataVersion, original.outer.points[0]);
    check(coast && coast->elevation < 1e-7, "Original coastline at sea level");
    auto interior =
        field->sample(f.sourceObjectId, f.dataVersion, {grid.origin.x + 1000, grid.origin.y + 500});
    check(interior && interior->elevation > 0, "Interior rises above sea level");
    auto again = field->sample(f.sourceObjectId, f.dataVersion, {grid.origin.x + 1000, grid.origin.y + 500});
    check(again->elevation == interior->elevation && again->normal == interior->normal,
          "Deterministic global source field");
    check(!field->sample(f.sourceObjectId, "wrong-version", grid.origin), "Version isolation");
    config.heightMeters = 84;
    IslandTerrain taller(config);
    taller.addDataset(data);
    check(
        std::abs(taller.sample(f.sourceObjectId, f.dataVersion, {grid.origin.x + 1000, grid.origin.y + 500})
                     ->elevation -
                 2 * interior->elevation) < 1e-9,
        "Configurable island height");
    check(taller.version() != field->version(), "Terrain configuration changes cache identity");
    auto noCoasts = data;
    auto& polygon = std::get<Polygon>(noCoasts.features[0].geometry);
    polygon.outer.edges.assign(polygon.outer.edges.size(), BoundaryKind::Unknown);
    for (auto& h : polygon.holes)
        h.edges.assign(h.edges.size(), BoundaryKind::Unknown);
    IslandTerrain unknown(config);
    unknown.addDataset(noCoasts);
    check(unknown.sample(f.sourceObjectId, f.dataVersion, {grid.origin.x + 1000, grid.origin.y + 500})
                  ->elevation == 0,
          "Unknown boundaries do not invent coast-based hills");
#ifdef VEYLAXIS_HAS_OSGB
    auto path = std::filesystem::temp_directory_path() /
                ("veylaxis-island-" +
                 contentHash(std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))
                     .substr(0, 12));
    ServiceConfig service;
    service.cacheDirectory = path.string();
    service.osgbEnabled = true;
    service.builder = proceduralBuilder(field);
    service.builderVersion = field->version();
    service.sourceLoaded = [field](const Dataset& source)
    {
        field->addDataset(source);
    };
    LocalService provider(data, service);
    using Key = std::pair<long long, long long>;
    using Sample = std::array<double, 4>;
    std::map<Key, Sample> boundaries[2];
    double maximum = 0;
    for (int side = 0; side < 2; ++side)
    {
        auto p = value(provider.request({"proc", {grid.id, side, 0}, {"land"}}));
        auto node = osgDB::readRefNodeFile(p.artifacts[0].path);
        check(node.valid(), "Procedural OSGB readable");

        struct Inspect : osg::NodeVisitor
        {
            XY origin;
            double seam;
            std::map<Key, Sample>& points;
            double& maximum;

            Inspect(XY o, double s, std::map<Key, Sample>& p, double& m)
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN), origin(o), seam(s), points(p), maximum(m)
            {
            }

            void apply(osg::Geode& geode) override
            {
                for (unsigned i = 0; i < geode.getNumDrawables(); ++i)
                {
                    auto* g = geode.getDrawable(i)->asGeometry();
                    auto* v = g ? dynamic_cast<osg::Vec3dArray*>(g->getVertexArray()) : nullptr;
                    if (!v)
                        continue;
                    if (g->getName() == "procedural-island-top")
                    {
                        auto* normals = dynamic_cast<osg::Vec3Array*>(g->getNormalArray());
                        check(normals && normals->size() == v->size(), "Per-vertex procedural normals");
                        for (std::size_t j = 0; j < v->size(); ++j)
                        {
                            auto p = (*v)[j];
                            maximum = std::max(maximum, p.z());
                            if (std::abs(p.x() + origin.x - seam) < 1e-7)
                            {
                                auto n = (*normals)[j];
                                points[{std::llround((p.x() + origin.x) * 1e6),
                                        std::llround((p.y() + origin.y) * 1e6)}] = {p.z(), n.x(), n.y(),
                                                                                    n.z()};
                            }
                        }
                    }
                    else if (g->getName() == "geographic-walls-only")
                        for (std::size_t j = 0; j + 3 < v->size(); j += 4)
                            check(!(std::abs((*v)[j].x() + origin.x - seam) < 1e-7 &&
                                    std::abs((*v)[j + 1].x() + origin.x - seam) < 1e-7),
                                  "No artificial seam wall");
                }
            }
        } visitor(p.metadata.tileOrigin, grid.origin.x + 1000, boundaries[side], maximum);

        node->accept(visitor);
    }
    check(maximum > 5 && maximum <= 42.00001, "Actual relief is not the flat display model");
    check(boundaries[0].size() > 10 && boundaries[0].size() == boundaries[1].size(),
          "Shared lattice segmentation matches");
    for (const auto& p : boundaries[0])
    {
        check(boundaries[1].count(p.first) == 1, "No T-junction or missing seam sample");
        for (int i = 0; i < 4; ++i)
            check(std::abs(p.second[i] - boundaries[1].at(p.first)[i]) < 1e-6,
                  "Shared seam height and normal agree");
    }
#endif
    std::cout << "VGDS S4.6: " << checks << " checks passed; unified source coast height and normals\n";
    return 0;
}
catch (const std::exception& e)
{
    std::cerr << "Island terrain failed: " << e.what() << '\n';
    return 1;
}
