// 【阅读导航】静态渲染缓存构建：把岛屿/海底数据转为 OSG 网格；航标和运动船只由主程序单独实例化。
#include "veylaxis/vgds/OsgBuilder.hpp"
#include "veylaxis/vgds/Service.hpp"
#include "veylaxis/vgds/Grid.hpp"
#include "OsgMesh.hpp"
#include <osg/Geode>
#include <osg/Geometry>
#include <osg/Group>
#include <osg/ValueObject>
#include <osgDB/WriteFile>
#include <mapbox/earcut.hpp>
#include <array>
#include <cmath>
#include <filesystem>
#include <stdexcept>
#include <algorithm>

namespace veylaxis::vgds
{
namespace
{
// 12 米仅用于未启用程序化岛屿的旧平顶分支；当前默认配置启用统一海岸距离高度场。
constexpr double height = 12, base = 0;

double signedArea(const Ring& r)
{
    double area = 0;
    auto origin = r.points.front();
    for (std::size_t i = 0; i < r.points.size(); ++i)
    {
        auto a = r.points[i], b = r.points[(i + 1) % r.points.size()];
        area += (a.x - origin.x) * (b.y - origin.y) - (b.x - origin.x) * (a.y - origin.y);
    }
    return area / 2;
}

osg::ref_ptr<osg::Geometry> surface(const Polygon& p, XY origin)
{
    using Coord = std::array<double, 2>;
    std::vector<std::vector<Coord>> rings;
    osg::ref_ptr<osg::Vec3dArray> vertices = new osg::Vec3dArray;
    auto add = [&](const Ring& r)
    {
        rings.emplace_back();
        for (auto a : r.points)
        {
            rings.back().push_back({a.x - origin.x, a.y - origin.y});
            vertices->push_back({a.x - origin.x, a.y - origin.y, height});
        }
    };
    add(p.outer);
    for (const auto& h : p.holes)
        add(h);
    const auto indices = mapbox::earcut<std::uint32_t>(rings);
    osg::ref_ptr<osg::DrawElementsUInt> triangles = new osg::DrawElementsUInt(GL_TRIANGLES);
    double area = 0;
    if (indices.empty() || indices.size() % 3)
        throw std::runtime_error("Earcut returned no triangles");
    for (std::size_t i = 0; i < indices.size(); i += 3)
    {
        auto a = indices[i], b = indices[i + 1], c = indices[i + 2];
        if (a >= vertices->size() || b >= vertices->size() || c >= vertices->size())
            throw std::runtime_error("Earcut index overflow");
        auto cross = ((*vertices)[b] - (*vertices)[a]) ^ ((*vertices)[c] - (*vertices)[a]);
        if (cross.z() < 0)
            std::swap(b, c);
        area += std::abs(cross.z()) / 2;
        triangles->push_back(a);
        triangles->push_back(b);
        triangles->push_back(c);
    }
    double expected = polygonArea(p);
    if (std::abs(area - expected) > std::max(1e-5, expected * 1e-9))
        throw std::runtime_error("Earcut area mismatch");
    osg::ref_ptr<osg::Geometry> out = new osg::Geometry;
    out->setName("estimated-flat-top");
    out->setVertexArray(vertices);
    out->addPrimitiveSet(triangles);
    osg::ref_ptr<osg::Vec3Array> normals = new osg::Vec3Array;
    normals->push_back({0, 0, 1});
    out->setNormalArray(normals, osg::Array::BIND_OVERALL);
    osg::ref_ptr<osg::Vec4Array> colors = new osg::Vec4Array;
    colors->push_back({.28f, .52f, .20f, 1});
    out->setColorArray(colors, osg::Array::BIND_OVERALL);
    out->setUseDisplayList(false);
    out->setUseVertexBufferObjects(true);
    return out;
}

osg::ref_ptr<osg::Geometry> walls(const Polygon& p, XY origin, const Feature* feature = nullptr,
                                  const std::shared_ptr<IslandTerrain>& field = {})
{
    osg::ref_ptr<osg::Vec3dArray> vertices = new osg::Vec3dArray;
    osg::ref_ptr<osg::Vec3Array> normals = new osg::Vec3Array;
    osg::ref_ptr<osg::DrawElementsUInt> triangles = new osg::DrawElementsUInt(GL_TRIANGLES);
    auto add = [&](const Ring& r, bool hole)
    {
        const double side = (signedArea(r) > 0 ? 1.0 : -1.0) * (hole ? -1.0 : 1.0);
        for (std::size_t i = 0; i < r.points.size(); ++i)
        {
            // 只给真实地理边建侧壁；人工切块边不能封墙，否则拼接后出现内部墙。
            if (r.edges[i] != BoundaryKind::Geographic)
                continue;
            auto a = r.points[i], b = r.points[(i + 1) % r.points.size()];
            osg::Vec3d normal((b.y - a.y) * side, -(b.x - a.x) * side, 0);
            if (normal.normalize() == 0)
                continue;
            double za = height, zb = height, bottom = base;
            if (field && feature)
            {
                auto sa = field->sample(feature->sourceObjectId, feature->dataVersion, a),
                     sb = field->sample(feature->sourceObjectId, feature->dataVersion, b);
                if (!sa || !sb)
                    throw std::runtime_error("Missing full island source context");
                za = sa->elevation;
                zb = sb->elevation;
                bottom = -field->config().baseDepthMeters;
            }
            unsigned k = static_cast<unsigned>(vertices->size());
            vertices->push_back({a.x - origin.x, a.y - origin.y, bottom});
            vertices->push_back({b.x - origin.x, b.y - origin.y, bottom});
            vertices->push_back({b.x - origin.x, b.y - origin.y, zb});
            vertices->push_back({a.x - origin.x, a.y - origin.y, za});
            for (int j = 0; j < 4; ++j)
                normals->push_back(osg::Vec3(normal));
            for (unsigned j : std::array<unsigned, 6>{0, 1, 2, 0, 2, 3})
                triangles->push_back(k + (side > 0 ? j : (j == 1 ? 3 : j == 3 ? 1 : j)));
        }
    };
    add(p.outer, false);
    for (const auto& h : p.holes)
        add(h, true);
    osg::ref_ptr<osg::Geometry> out = new osg::Geometry;
    out->setName("geographic-walls-only");
    out->setVertexArray(vertices);
    out->setNormalArray(normals, osg::Array::BIND_PER_VERTEX);
    out->addPrimitiveSet(triangles);
    osg::ref_ptr<osg::Vec4Array> colors = new osg::Vec4Array;
    colors->push_back({.35f, .30f, .19f, 1});
    out->setColorArray(colors, osg::Array::BIND_OVERALL);
    out->setUseDisplayList(false);
    out->setUseVertexBufferObjects(true);
    return out;
}

osg::ref_ptr<osg::Geometry> proceduralSurface(const Feature& feature, const TilePackage& package,
                                              const std::shared_ptr<IslandTerrain>& field)
{
    const auto origin = package.metadata.tileOrigin;
    const auto& grid = package.metadata.grid;
    double step = field->config().sampleSpacingMeters;
    auto count = nodeCount(grid, step);
    if (auto e = std::get_if<TileError>(&count))
        throw std::runtime_error(e->message);
    auto cells = std::get<std::uint32_t>(count) - 1;
    auto bounds = geometryBounds(feature.geometry);
    osg::ref_ptr<osg::Vec3dArray> vertices = new osg::Vec3dArray;
    osg::ref_ptr<osg::Vec3Array> normals = new osg::Vec3Array;
    osg::ref_ptr<osg::Vec4Array> colors = new osg::Vec4Array;
    osg::ref_ptr<osg::DrawElementsUInt> triangles = new osg::DrawElementsUInt(GL_TRIANGLES);
    auto vertex = [&](XY p)
    {
        auto sample = field->sample(feature.sourceObjectId, feature.dataVersion, p);
        if (!sample)
            throw std::runtime_error("Missing original source for procedural terrain");
        vertices->push_back({p.x - origin.x, p.y - origin.y, sample->elevation});
        normals->push_back({static_cast<float>(sample->normal[0]), static_cast<float>(sample->normal[1]),
                            static_cast<float>(sample->normal[2])});
        float t = static_cast<float>(std::min(1.0, sample->elevation / field->config().heightMeters * 4));
        colors->push_back(
            {.65f * (1 - t) + .20f * t, .58f * (1 - t) + .50f * t, .34f * (1 - t) + .15f * t, 1});
    };
    auto polygon = [&](const Polygon& p)
    {
        std::vector<std::vector<std::array<double, 2>>> rings;
        std::vector<XY> points;
        auto add = [&](const Ring& r)
        {
            rings.emplace_back();
            for (auto v : r.points)
            {
                points.push_back(v);
                rings.back().push_back({v.x - origin.x, v.y - origin.y});
            }
        };
        add(p.outer);
        for (const auto& h : p.holes)
            add(h);
        auto indices = mapbox::earcut<std::uint32_t>(rings);
        for (std::size_t j = 0; j < indices.size(); j += 3)
        {
            auto a = points.at(indices[j]), b = points.at(indices[j + 1]), c = points.at(indices[j + 2]);
            if ((b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x) < 0)
                std::swap(b, c);
            auto start = static_cast<unsigned>(vertices->size());
            vertex(a);
            vertex(b);
            vertex(c);
            vertex({(a.x + b.x + c.x) / 3, (a.y + b.y + c.y) / 3});
            for (unsigned k : std::array<unsigned, 9>{0, 1, 3, 1, 2, 3, 2, 0, 3})
                triangles->push_back(start + k);
        }
    };
    // Shared global lattice lines and original contour intersections define all
    // seam vertices. Earcut subdivisions never invent tile-specific seam nodes.
    const auto gx = package.metadata.tile.x * static_cast<std::int64_t>(cells),
               gy = package.metadata.tile.y * static_cast<std::int64_t>(cells);
    for (std::uint32_t y = 0; y < cells; ++y)
        for (std::uint32_t x = 0; x < cells; ++x)
        {
            requestCheckpoint();
            Bounds b{grid.origin.x + (gx + x) * step, grid.origin.y + (gy + y) * step,
                     grid.origin.x + (gx + x + 1) * step, grid.origin.y + (gy + y + 1) * step};
            if (b.maxX < bounds.minX || b.minX > bounds.maxX || b.maxY < bounds.minY || b.minY > bounds.maxY)
                continue;
            auto result = clipFeature(feature, b, package.metadata.tile);
            if (auto e = std::get_if<TileError>(&result))
                throw std::runtime_error(e->message);
            for (const auto& f : std::get<std::vector<Feature>>(result))
            {
                if (auto p = std::get_if<Polygon>(&f.geometry))
                    polygon(*p);
                else if (auto m = std::get_if<MultiPolygon>(&f.geometry))
                    for (const auto& part : m->polygons)
                        polygon(part);
            }
        }
    osg::ref_ptr<osg::Geometry> geometry = new osg::Geometry;
    geometry->setName("procedural-island-top");
    geometry->setVertexArray(vertices);
    geometry->setNormalArray(normals, osg::Array::BIND_PER_VERTEX);
    geometry->setColorArray(colors, osg::Array::BIND_PER_VERTEX);
    geometry->addPrimitiveSet(triangles);
    geometry->setUseDisplayList(false);
    geometry->setUseVertexBufferObjects(true);
    return geometry;
}
}

osg::ref_ptr<osg::Node> createLandNode(const TilePackage& p)
{
    return createLandNode(p, {});
}

osg::ref_ptr<osg::Node> createLandNode(const TilePackage& p, const std::shared_ptr<IslandTerrain>& field)
{
    if (auto e = validate(p))
        throw std::invalid_argument(e->message);
    osg::ref_ptr<osg::Group> root = new osg::Group;
    root->setName("VGDS regenerable land cache");
    auto origin = p.metadata.tileOrigin;
    root->setUserValue("origin_easting", origin.x);
    root->setUserValue("origin_northing", origin.y);
    root->setUserValue("vertical_scale", 1.0);
    root->setUserValue("height_provenance",
                       field ? std::string("Synthetic coast-distance terrain; not ENC elevation")
                             : std::string("Estimated flat 12m; display only; not ENC elevation"));
    root->setUserValue("builder_version", field ? field->version() : std::string(OsgBuilderVersion));
    if (p.terrain)
    {
        const auto& t = *p.terrain;
        osg::ref_ptr<osg::Vec3dArray> vertices = new osg::Vec3dArray;
        osg::ref_ptr<osg::Vec3Array> normals = new osg::Vec3Array;
        osg::ref_ptr<osg::Vec4Array> colors = new osg::Vec4Array;
        osg::ref_ptr<osg::DrawElementsUInt> triangles = new osg::DrawElementsUInt(GL_TRIANGLES);
        for (std::uint32_t y = 0; y < t.rows; ++y)
            for (std::uint32_t x = 0; x < t.columns; ++x)
            {
                auto i = static_cast<std::size_t>(y) * t.columns + x;
                double z = t.valid[i] ? t.elevations[i] : 0;
                vertices->push_back({t.northWestSample.x + x * t.spacingMeters - origin.x,
                                     t.northWestSample.y - y * t.spacingMeters - origin.y, z});
                if (t.normals.empty())
                    normals->push_back({0, 0, 1});
                else
                    normals->push_back({t.normals[i][0], t.normals[i][1], t.normals[i][2]});
                float depth = static_cast<float>(std::clamp((z + 50) / 50, 0.0, 1.0));
                colors->push_back({.08f + .45f * depth, .18f + .40f * depth, .50f - .25f * depth, 1});
            }
        auto triangle = [&](unsigned a, unsigned b, unsigned c)
        {
            if (t.valid[a] && t.valid[b] && t.valid[c])
            {
                triangles->push_back(a);
                triangles->push_back(b);
                triangles->push_back(c);
            }
        };
        for (unsigned y = 0; y + 1 < t.rows; ++y)
            for (unsigned x = 0; x + 1 < t.columns; ++x)
            {
                unsigned a = y * t.columns + x, b = a + 1, c = a + t.columns, d = c + 1;
                triangle(a, c, b);
                triangle(b, c, d);
            }
        osg::ref_ptr<osg::Geometry> mesh = new osg::Geometry;
        mesh->setName("estimated-ENC-seabed");
        mesh->setVertexArray(vertices);
        mesh->setNormalArray(normals, osg::Array::BIND_PER_VERTEX);
        mesh->setColorArray(colors, osg::Array::BIND_PER_VERTEX);
        mesh->addPrimitiveSet(triangles);
        mesh->setUseDisplayList(false);
        mesh->setUseVertexBufferObjects(true);
        osg::ref_ptr<osg::Geode> node = new osg::Geode;
        node->addDrawable(mesh);
        node->setUserValue("vertical_reference", t.verticalDatum);
        root->addChild(node);
    }
    for (const auto& f : p.features)
    {
        if (f.layer != "land")
            continue;
        osg::ref_ptr<osg::Geode> node = new osg::Geode;
        node->setName(f.fragmentId);
        node->setUserValue("source_id", f.sourceObjectId);
        node->setUserValue("data_version", f.dataVersion);
        if (field &&
            (std::holds_alternative<Polygon>(f.geometry) || std::holds_alternative<MultiPolygon>(f.geometry)))
            node->addDrawable(proceduralSurface(f, p, field));
        auto add = [&](const Polygon& polygon)
        {
            if (!field)
                node->addDrawable(surface(polygon, origin));
            node->addDrawable(walls(polygon, origin, &f, field));
        };
        if (auto v = std::get_if<Polygon>(&f.geometry))
            add(*v);
        else if (auto m = std::get_if<MultiPolygon>(&f.geometry))
            for (const auto& part : m->polygons)
                add(part);
        root->addChild(node);
    }
    return root;
}

// 这里输出的是可再生成的静态渲染缓存；原始几何、来源和高程仍保留在数据包中。
RenderArtifact buildOsgb(const TilePackage& p, const std::string& path)
{
    auto root = createLandNode(p);
    const auto origin = p.metadata.tileOrigin;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    auto temporary = path + ".tmp.osgb";
    if (!osgDB::writeNodeFile(*root, temporary))
        throw std::runtime_error("OSGB write failed");
    if (std::filesystem::exists(path))
        std::filesystem::remove(path);
    std::filesystem::rename(temporary, path);
    RenderArtifact a;
    a.path = std::filesystem::absolute(path).string();
    a.builderVersion = OsgBuilderVersion;
    a.origin = origin;
    a.bytes = std::filesystem::file_size(path);
    a.contentHash = fileHash(path);
    return a;
}

ArtifactBuilder proceduralBuilder(std::shared_ptr<IslandTerrain> field)
{
    if (!field)
        throw std::invalid_argument("Missing island terrain context");
    return [field = std::move(field)](const TilePackage& p, const std::string& path)
    {
        auto node = createLandNode(p, field);
        auto temporary = path + ".tmp.osgb";
        std::filesystem::create_directories(std::filesystem::path(path).parent_path());
        if (!osgDB::writeNodeFile(*node, temporary))
            throw std::runtime_error("Procedural OSGB write failed");
        if (std::filesystem::exists(path))
            std::filesystem::remove(path);
        std::filesystem::rename(temporary, path);
        RenderArtifact artifact;
        artifact.path = std::filesystem::absolute(path).string();
        artifact.builderVersion = field->version();
        artifact.origin = p.metadata.tileOrigin;
        artifact.bytes = std::filesystem::file_size(path);
        artifact.contentHash = fileHash(path);
        return artifact;
    };
}
}
