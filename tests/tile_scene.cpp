#include <cpl_json.h>
#include <osg/Geode>
#include <osg/Geometry>
#include <osg/MatrixTransform>
#include <osg/ValueObject>
#include <osgDB/ReadFile>
#include <osgDB/WriteFile>
#include <osgDB/Options>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <map>
#include <stdexcept>

int runTileScene(int argc, char** argv)
try
{
    auto require = [](bool b, const char* m)
    {
        if (!b)
            throw std::runtime_error(m);
    };
    require(argc == 3, "Expected manifest.json and output.osgb");
    namespace fs = std::filesystem;
    CPLJSONDocument manifest;
    require(manifest.Load(argv[1]), "Cannot load tile manifest");
    auto input = manifest.GetRoot();
    auto origin = input.GetArray("origin");
    const double originX = origin[0].ToDouble(), originY = origin[1].ToDouble();
    osg::ref_ptr<osg::Group> group = new osg::Group;
    group->setName("Nine independently reconstructed 1km seabed tiles");
    group->setUserValue("origin_easting", originX);
    group->setUserValue("origin_northing", originY);
    group->setUserValue("vertical_scale", 1.0);
    group->setUserValue("vertical_offset_m", 0.0);

    struct Tile
    {
        osg::ref_ptr<osg::Node> node;
        osg::Vec3Array* vertices;
        osg::Vec3Array* normals;
        osg::Vec4Array* colors;
        double x, y;
    };

    std::map<std::pair<int, int>, Tile> tiles;
    for (const auto& entry : input.GetArray("tiles"))
    {
        auto path = fs::path(argv[1]).parent_path() / entry.GetString("folder") / "region001_terrain.osgb";
        auto node = osgDB::readRefNodeFile(path.string());
        require(node.valid(), "Cannot load independent tile model");
        auto* geode = dynamic_cast<osg::Geode*>(node.get());
        require(geode && geode->getNumDrawables() == 1, "Invalid tile geode");
        auto* geometry = dynamic_cast<osg::Geometry*>(geode->getDrawable(0));
        require(geometry, "Missing tile mesh");
        auto* vertices = dynamic_cast<osg::Vec3Array*>(geometry->getVertexArray());
        auto* normals = dynamic_cast<osg::Vec3Array*>(geometry->getNormalArray());
        auto* colors = dynamic_cast<osg::Vec4Array*>(geometry->getColorArray());
        require(vertices && normals && colors && vertices->size() == 40401 && normals->size() == 40401,
                "Expected 201x201 tile nodes and normals");
        double x = 0, y = 0, scale = 0;
        require(node->getUserValue("origin_easting", x) && node->getUserValue("origin_northing", y) &&
                    node->getUserValue("vertical_scale", scale) && scale == 1,
                "Missing true-scale tile origin");
        std::string wkt;
        require(node->getUserValue("horizontal_crs_wkt", wkt), "Missing CRS");
        group->setUserValue("horizontal_crs_wkt", wkt);
        osg::ref_ptr<osg::MatrixTransform> placement = new osg::MatrixTransform;
        placement->setName(entry.GetString("folder"));
        placement->setMatrix(osg::Matrixd::translate(x - originX, y - originY, 0));
        placement->addChild(node);
        group->addChild(placement);
        tiles[{entry.GetInteger("x"), entry.GetInteger("y")}] = Tile{node, vertices, normals, colors, x, y};
    }
    double maxXY = 0, maxZ = 0, maxAngle = 0, maxColor = 0;
    int pairs = 0;
    for (auto& item : tiles)
        for (const auto step : {std::pair<int, int>{1, 0}, {0, 1}})
        {
            auto neighbor = tiles.find({item.first.first + step.first, item.first.second + step.second});
            if (neighbor == tiles.end())
                continue;
            const auto& a = item.second;
            const auto& b = neighbor->second;
            for (int p = 0; p < 201; ++p)
            {
                const int ai = step.first ? p * 201 + 200 : p, bi = step.first ? p * 201 : 200 * 201 + p;
                osg::Vec3d av = (*a.vertices)[ai], bv = (*b.vertices)[bi];
                av += osg::Vec3d(a.x - originX, a.y - originY, 0);
                bv += osg::Vec3d(b.x - originX, b.y - originY, 0);
                maxXY = std::max(maxXY, std::hypot(av.x() - bv.x(), av.y() - bv.y()));
                maxZ = std::max(maxZ, std::abs(av.z() - bv.z()));
                osg::Vec3d an = (*a.normals)[ai], bn = (*b.normals)[bi];
                an.normalize();
                bn.normalize();
                maxAngle =
                    std::max(maxAngle, std::acos(std::clamp(an * bn, -1.0, 1.0)) * 180 / 3.141592653589793);
                maxColor =
                    std::max(maxColor, static_cast<double>(((*a.colors)[ai] - (*b.colors)[bi]).length()));
                ++pairs;
            }
        }
    osg::ref_ptr<osgDB::Options> options = new osgDB::Options("precision 17");
    require(osgDB::writeNodeFile(*group, argv[2], options), "Cannot save assembled independent scene");
    CPLJSONObject report;
    report.Add("tiles", static_cast<int>(tiles.size()));
    report.Add("compared_shared_vertices", pairs);
    report.Add("max_xy_gap_m", maxXY);
    report.Add("max_height_gap_m", maxZ);
    report.Add("max_normal_angle_deg", maxAngle);
    report.Add("max_color_difference", maxColor);
    const bool passed = tiles.size() == 9 && pairs == 2412 && maxXY < 1e-5 && maxZ <= .001 &&
                        maxAngle <= .1 && maxColor < .001;
    report.Add("geometry_and_shading_pass", passed);
    CPLJSONDocument output;
    output.SetRoot(report);
    require(output.Save((fs::path(argv[2]).parent_path() / "model-seams.json").string()),
            "Cannot write mesh seam report");
    std::cout << "Independent tile scene: XY gap " << maxXY << " m; Z gap " << maxZ << " m; normals "
              << maxAngle << " deg; color delta " << maxColor << "; pass=" << passed << '\n';
    return passed ? 0 : 2;
}
catch (const std::exception& e)
{
    std::cerr << e.what() << '\n';
    return 1;
}
