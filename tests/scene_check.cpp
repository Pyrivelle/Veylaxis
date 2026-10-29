#include <gdal_priv.h>
#include <osg/Geode>
#include <osg/Geometry>
#include <osg/ValueObject>
#include <osgDB/ReadFile>
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

int runSceneCheck(int argc, char** argv)
try
{
    auto require = [](bool ok, const char* message)
    {
        if (!ok)
            throw std::runtime_error(message);
    };
    require(argc == 3, "Expected scene and elevation raster paths");
    osg::ref_ptr<osg::Node> node = osgDB::readRefNodeFile(argv[1]);
    require(node.valid(), "Cannot reopen OSG scene");
    auto* geode = dynamic_cast<osg::Geode*>(node.get());
    require(geode && geode->getNumDrawables() == 1, "Expected one seabed surface");
    auto* geometry = dynamic_cast<osg::Geometry*>(geode->getDrawable(0));
    require(geometry, "Missing terrain geometry");
    auto* vertices = dynamic_cast<osg::Vec3Array*>(geometry->getVertexArray());
    double originX = 0, originY = 0, scale = 0;
    require(node->getUserValue("origin_easting", originX) && node->getUserValue("origin_northing", originY) &&
                node->getUserValue("vertical_scale", scale),
            "Missing georeferencing metadata");
    require(scale == 1.0, "Model must have true 1:1 vertical scale");
    GDALAllRegister();
    auto close = [](GDALDataset* d)
    {
        if (d)
            GDALClose(d);
    };
    std::unique_ptr<GDALDataset, decltype(close)> raster(
        static_cast<GDALDataset*>(GDALOpen(argv[2], GA_ReadOnly)), close);
    require(bool(raster), "Cannot open reference raster");
    const int w = raster->GetRasterXSize(), h = raster->GetRasterYSize();
    require(vertices && vertices->size() == static_cast<size_t>(w) * h, "Incorrect model grid");
    double gt[6];
    require(raster->GetGeoTransform(gt) == CE_None, "Missing raster transform");
    auto* band = raster->GetRasterBand(1);
    double nodata = band->GetNoDataValue();
    std::vector<float> z(vertices->size());
    require(band->RasterIO(GF_Read, 0, 0, w, h, z.data(), w, h, GDT_Float32, 0, 0) == CE_None,
            "Cannot read elevations");
    for (int r = 0; r < h; ++r)
        for (int c = 0; c < w; ++c)
        {
            const size_t i = static_cast<size_t>(r) * w + c;
            const auto p = (*vertices)[i];
            const double x = gt[0] + (c + 0.5) * gt[1] + (r + 0.5) * gt[2] - originX;
            const double y = gt[3] + (c + 0.5) * gt[4] + (r + 0.5) * gt[5] - originY;
            require(std::abs(p.x() - x) < 1e-4 && std::abs(p.y() - y) < 1e-4,
                    "Horizontal model scale is not 1 metre per unit");
            if (z[i] != nodata)
                require(std::abs(p.z() - z[i]) < 1e-4, "Vertical model differs from metre elevations");
        }
    require(geometry->getNumPrimitiveSets() == 1, "Expected indexed mesh");
    auto* triangles = dynamic_cast<osg::DrawElementsUInt*>(geometry->getPrimitiveSet(0));
    require(triangles && triangles->size() > 0 && triangles->size() % 3 == 0, "Missing mesh triangles");
    for (auto index : *triangles)
        require(index < z.size() && z[index] != nodata, "Mesh includes invalid vertex");
    std::cout << "OSG model verified: " << vertices->size() << " vertices, " << triangles->size() / 3
              << " triangles; XYZ units metres, scale 1:1, raster heights preserved\n";
    return 0;
}
catch (const std::exception& e)
{
    std::cerr << e.what() << '\n';
    return 1;
}
