// 【阅读导航】通用显示外壳：离线地形查看、共享 OSG 相机、天空海洋接入和截图；动态场景也调用 showTerrain()。
#include <gdal_priv.h>
#include <ogr_spatialref.h>
#include <osg/Geode>
#include <osg/Geometry>
#include <osg/ValueObject>
#include <osgDB/WriteFile>
#include <osgDB/ReadFile>
#include <osgDB/Options>
#include "environment/MarineEnvironment.h"
#include "CameraNavigation.h"
#include <osg/Material>
#include <chrono>
#include <filesystem>
#include <osgGA/TrackballManipulator>
#include <osgGA/StateSetManipulator>
#include <osgUtil/SmoothingVisitor>
#include <osgViewer/Viewer>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

// 动态与离线入口在这里汇合；root 已是渲染节点，此处不再解析 S-57。
int showTerrain(osg::Node* root, double originX, double originY, const OGRSpatialReference& terrainSrs,
                double scale, bool marine, const std::string& smokePath,
                osgGA::GUIEventHandler* extraHandler = nullptr);

namespace
{
void surfaceView(osgGA::TrackballManipulator* manipulator)
{
    manipulator->setHomePosition({350, -700, 60}, {0, 300, -5}, {0, 0, 1}, false);
    manipulator->home(0.0);
}

void underwaterView(osgGA::TrackballManipulator* manipulator)
{
    manipulator->setHomePosition({-300, -250, -12}, {-20, 250, -20}, {0, 0, 1}, false);
    manipulator->home(0.0);
}

class MarineKeys final : public osgGA::GUIEventHandler
{
  public:
    MarineKeys(std::shared_ptr<MarineEnvironment> environment, osgGA::TrackballManipulator* manipulator)
        : environment_(std::move(environment)), manipulator_(manipulator)
    {
    }

    bool handle(const osgGA::GUIEventAdapter& event, osgGA::GUIActionAdapter&) override
    {
        if (event.getEventType() != osgGA::GUIEventAdapter::KEYDOWN)
            return false;
        if (event.getKey() == osgGA::GUIEventAdapter::KEY_F1)
        {
            surfaceView(manipulator_);
            return true;
        }
        if (event.getKey() == osgGA::GUIEventAdapter::KEY_F2)
        {
            underwaterView(manipulator_);
            return true;
        }
        if (event.getKey() == 'o' || event.getKey() == 'O')
        {
            environment_->toggleOcean();
            return true;
        }
        return false;
    }

  private:
    std::shared_ptr<MarineEnvironment> environment_;
    osg::ref_ptr<osgGA::TrackballManipulator> manipulator_;
};

class CaptureFrame final : public osg::Camera::DrawCallback
{
  public:
    mutable bool requested = false;
    mutable osg::ref_ptr<osg::Image> image;

    void operator()(osg::RenderInfo& info) const override
    {
        if (!requested)
            return;
        auto* viewport = info.getCurrentCamera()->getViewport();
        image = new osg::Image;
        glReadBuffer(GL_BACK);
        image->readPixels(0, 0, static_cast<int>(viewport->width()), static_cast<int>(viewport->height()),
                          GL_RGB, GL_UNSIGNED_BYTE);
        requested = false;
    }
};
}

int runViewer(int argc, char** argv)
try
{
    if (argc < 2)
    {
        std::cout
            << "Veylaxis view elevation.tif|terrain.osgb [--export terrain.osgt] [--z-scale factor] [--marine on|off] [--smoke-test preview.png] [--color-range min max]\nDefault: integrated marine scene, XYZ metres, Z x1.\n";
        return 0;
    }
    std::string exportPath, smokePath;
    double scale = 1, colorMinimum = 0, colorMaximum = 0;
    bool marine = true, fixedColors = false, cachedNormals = true;
    for (int i = 2; i < argc; ++i)
    {
        std::string a = argv[i];
        if (i + 1 >= argc)
            throw std::runtime_error("Missing option value");
        if (a == "--export")
            exportPath = argv[++i];
        else if (a == "--z-scale")
            scale = std::stod(argv[++i]);
        else if (a == "--marine")
        {
            std::string mode = argv[++i];
            if (mode != "on" && mode != "off")
                throw std::runtime_error("--marine requires on or off");
            marine = mode == "on";
        }
        else if (a == "--smoke-test")
            smokePath = argv[++i];
        else if (a == "--normal-mode")
        {
            std::string mode = argv[++i];
            if (mode != "local" && mode != "cached")
                throw std::runtime_error("--normal-mode requires local or cached");
            cachedNormals = mode == "cached";
        }
        else if (a == "--color-range")
        {
            colorMinimum = std::stod(argv[++i]);
            if (i + 1 >= argc)
                throw std::runtime_error("--color-range needs min and max");
            colorMaximum = std::stod(argv[++i]);
            fixedColors = true;
        }
        else
            throw std::runtime_error("Unknown option");
    }
    if (!std::isfinite(scale) || scale <= 0)
        throw std::runtime_error("Invalid z-scale");
    if (marine && exportPath.empty() && scale != 1.0)
        throw std::runtime_error(
            "Integrated marine scene requires --z-scale 1; use --marine off for exaggerated seabed inspection");
    if (fixedColors &&
        (!std::isfinite(colorMinimum) || !std::isfinite(colorMaximum) || colorMinimum >= colorMaximum))
        throw std::runtime_error("Invalid color range");
    const auto extension = std::filesystem::path(argv[1]).extension().string();
    if (extension == ".osgb" || extension == ".osgt")
    {
        auto model = osgDB::readRefNodeFile(argv[1]);
        if (!model)
            throw std::runtime_error("Cannot load saved terrain scene");
        double x = 0, y = 0, storedScale = 0;
        std::string wkt;
        if (!model->getUserValue("origin_easting", x) || !model->getUserValue("origin_northing", y) ||
            !model->getUserValue("horizontal_crs_wkt", wkt) ||
            !model->getUserValue("vertical_scale", storedScale))
            throw std::runtime_error("Saved scene lacks origin/CRS metadata");
        OGRSpatialReference spatial;
        if (spatial.SetFromUserInput(wkt.c_str()) != OGRERR_NONE)
            throw std::runtime_error("Invalid saved terrain CRS");
        if (storedScale != scale)
            throw std::runtime_error("Saved scene scale does not match requested scale");
        if (!exportPath.empty())
        {
            osg::ref_ptr<osgDB::Options> options = new osgDB::Options("precision 17");
            if (!osgDB::writeNodeFile(*model, exportPath, options))
                throw std::runtime_error("Cannot export saved scene");
            return 0;
        }
        return showTerrain(model, x, y, spatial, scale, marine, smokePath);
    }
    GDALAllRegister();
    auto close = [](GDALDataset* d)
    {
        if (d)
            GDALClose(d);
    };
    std::unique_ptr<GDALDataset, decltype(close)> ds(
        static_cast<GDALDataset*>(GDALOpen(argv[1], GA_ReadOnly)), close);
    if (!ds)
        throw std::runtime_error("Cannot open raster");
    double gt[6];
    if (ds->GetGeoTransform(gt) != CE_None || !ds->GetSpatialRef())
        throw std::runtime_error("Raster must have georeferencing");
    if (!ds->GetSpatialRef()->IsProjected() || std::abs(ds->GetSpatialRef()->GetLinearUnits() - 1.0) > 1e-9)
        throw std::runtime_error("Viewer requires a projected raster with horizontal units in metres");
    const int w = ds->GetRasterXSize(), h = ds->GetRasterYSize();
    if (w < 2 || h < 2 || static_cast<double>(w) * h > 4000000)
        throw std::runtime_error("Grid must have 2..4 million cells");
    auto* band = ds->GetRasterBand(1);
    int hasNoData = 0;
    const double noData = band->GetNoDataValue(&hasNoData);
    std::vector<float> z(static_cast<size_t>(w) * h);
    if (band->RasterIO(GF_Read, 0, 0, w, h, z.data(), w, h, GDT_Float32, 0, 0) != CE_None)
        throw std::runtime_error("Cannot read raster");
    double originX = gt[0] + w * 0.5 * gt[1] + h * 0.5 * gt[2],
           originY = gt[3] + w * 0.5 * gt[4] + h * 0.5 * gt[5];
    osg::ref_ptr<osg::Vec3Array> vertices = new osg::Vec3Array;
    osg::ref_ptr<osg::Vec4Array> colors = new osg::Vec4Array;
    std::vector<bool> valid(z.size());
    float minimum = 1e30f, maximum = -1e30f;
    for (size_t i = 0; i < z.size(); ++i)
    {
        valid[i] = std::isfinite(z[i]) && (!hasNoData || z[i] != noData);
        if (valid[i])
        {
            minimum = std::min(minimum, z[i]);
            maximum = std::max(maximum, z[i]);
        }
    }
    if (minimum > maximum)
        throw std::runtime_error("No valid elevations");
    if (!fixedColors)
    {
        colorMinimum = minimum;
        colorMaximum = maximum;
    }
    for (int r = 0; r < h; ++r)
        for (int c = 0; c < w; ++c)
        {
            size_t i = static_cast<size_t>(r) * w + c;
            vertices->push_back(
                osg::Vec3(static_cast<float>(gt[0] + (c + 0.5) * gt[1] + (r + 0.5) * gt[2] - originX),
                          static_cast<float>(gt[3] + (c + 0.5) * gt[4] + (r + 0.5) * gt[5] - originY),
                          valid[i] ? static_cast<float>(z[i] * scale) : 0));
            float t =
                valid[i]
                    ? static_cast<float>(std::clamp(
                          (z[i] - colorMinimum) / std::max(colorMaximum - colorMinimum, 0.001), 0.0, 1.0))
                    : 0;
            colors->push_back(osg::Vec4(0.08f + 0.7f * t, 0.20f + 0.55f * t, 0.55f - 0.35f * t, 1));
        }
    osg::ref_ptr<osg::DrawElementsUInt> triangles = new osg::DrawElementsUInt(GL_TRIANGLES);
    auto triangle = [&](unsigned a, unsigned b, unsigned c)
    {
        if (valid[a] && valid[b] && valid[c])
        {
            triangles->push_back(a);
            triangles->push_back(b);
            triangles->push_back(c);
        }
    };
    for (int r = 0; r < h - 1; ++r)
        for (int c = 0; c < w - 1; ++c)
        {
            unsigned a = static_cast<unsigned>(r * w + c), b = a + 1, d = a + w, e = d + 1;
            triangle(a, d, b);
            triangle(b, d, e);
        }
    osg::ref_ptr<osg::Geometry> geometry = new osg::Geometry;
    geometry->setVertexArray(vertices);
    geometry->setColorArray(colors, osg::Array::BIND_PER_VERTEX);
    geometry->addPrimitiveSet(triangles);
    geometry->setUseDisplayList(false);
    geometry->setUseVertexBufferObjects(true);
    osgUtil::SmoothingVisitor::smooth(*geometry);
    if (const char* normalFile = ds->GetMetadataItem("NORMALS_FILE"); normalFile && cachedNormals)
    {
        auto normalPath = std::filesystem::path(argv[1]).parent_path() / normalFile;
        std::unique_ptr<GDALDataset, decltype(close)> normals(
            static_cast<GDALDataset*>(GDALOpen(normalPath.string().c_str(), GA_ReadOnly)), close);
        if (!normals || normals->GetRasterXSize() != w || normals->GetRasterYSize() != h ||
            normals->GetRasterCount() != 3)
            throw std::runtime_error("Invalid tile normal raster");
        double normalGt[6];
        if (normals->GetGeoTransform(normalGt) != CE_None)
            throw std::runtime_error("Missing normal georeferencing");
        for (int k = 0; k < 6; ++k)
            if (std::abs(normalGt[k] - gt[k]) > 1e-8)
                throw std::runtime_error("Normal raster is not aligned");
        std::vector<float> nx(z.size()), ny(z.size()), nz(z.size());
        std::vector<float>* channels[] = {&nx, &ny, &nz};
        for (int b = 0; b < 3; ++b)
            if (normals->GetRasterBand(b + 1)->RasterIO(GF_Read, 0, 0, w, h, channels[b]->data(), w, h,
                                                        GDT_Float32, 0, 0) != CE_None)
                throw std::runtime_error("Cannot read normal raster");
        osg::ref_ptr<osg::Vec3Array> perVertex = new osg::Vec3Array;
        for (size_t i = 0; i < z.size(); ++i)
        {
            osg::Vec3 n(nx[i], ny[i], static_cast<float>(nz[i] / scale));
            if (!valid[i])
                n.set(0, 0, 1);
            else if (!std::isfinite(n.x()) || !std::isfinite(n.y()) || !std::isfinite(n.z()) ||
                     n.length2() < .01f)
                throw std::runtime_error("Invalid vertex normal");
            n.normalize();
            perVertex->push_back(n);
        }
        geometry->setNormalArray(perVertex, osg::Array::BIND_PER_VERTEX);
    }
    osg::ref_ptr<osg::Geode> root = new osg::Geode;
    root->addDrawable(geometry);
    root->setName("Region001 seabed (metres)");
    osg::ref_ptr<osg::Material> material = new osg::Material;
    material->setColorMode(osg::Material::AMBIENT_AND_DIFFUSE);
    root->getOrCreateStateSet()->setAttributeAndModes(material);
    root->setUserValue("origin_easting", originX);
    root->setUserValue("origin_northing", originY);
    root->setUserValue("horizontal_crs_wkt", std::string(ds->GetProjectionRef()));
    root->setUserValue("vertical_scale", scale);
    root->setUserValue("vertical_offset_m", 0.0);
    root->setUserValue("vertical_reference", std::string("ENC sounding datum; elevation = -depth"));
    std::cout << "Mesh: " << vertices->size() << " vertices, " << triangles->size() / 3
              << " triangles; local XY origin " << std::fixed << originX << ", " << originY << "; z-scale "
              << scale << '\n';
    std::cout << "Seabed Z=" << minimum * scale << " .. " << maximum * scale
              << " m; vertical translation=0; marine sea datum Z=0 m\n";
    if (!exportPath.empty())
    {
        // Text OSG otherwise rounds large double UTM origins to six digits.
        osg::ref_ptr<osgDB::Options> options = new osgDB::Options("precision 17");
        if (!osgDB::writeNodeFile(*root, exportPath, options))
            throw std::runtime_error("Cannot export OSG scene");
        return 0;
    }
    return showTerrain(root, originX, originY, *ds->GetSpatialRef(), scale, marine, smokePath);
}
catch (const std::exception& e)
{
    std::cerr << "Error: " << e.what() << '\n';
    return 1;
}

int showTerrain(osg::Node* root, double originX, double originY, const OGRSpatialReference& terrainSrs,
                double scale, bool marine, const std::string& smokePath, osgGA::GUIEventHandler* extraHandler)
{
    osgViewer::Viewer viewer;
    viewer.setThreadingModel(osgViewer::Viewer::SingleThreaded);
    viewer.setReleaseContextAtEndOfFrameHint(false);
    osg::ref_ptr<osg::Group> scene = new osg::Group;
    scene->setName("Veylaxis integrated marine scene");
    scene->addChild(root);
    viewer.setSceneData(scene);
    osg::ref_ptr<osgGA::TrackballManipulator> manipulator = new ZUpManipulator;
    const osg::Vec3d center = root->getBound().center();
    const double radius = root->getBound().radius();
    manipulator->setHomePosition(center + osg::Vec3d(0.85 * radius, -1.3 * radius, 0.95 * radius), center,
                                 osg::Vec3d(0, 0, 1), false);
    viewer.setCameraManipulator(manipulator);
    if (extraHandler)
        viewer.addEventHandler(extraHandler);
    viewer.addEventHandler(new osgGA::StateSetManipulator(root->getOrCreateStateSet()));
    viewer.getCamera()->setClearColor(osg::Vec4(0.055f, 0.075f, 0.10f, 1));
    viewer.getCamera()->setComputeNearFarMode(osg::CullSettings::DO_NOT_COMPUTE_NEAR_FAR);
    viewer.getCamera()->setProjectionMatrixAsPerspective(45.0, 1.5, 0.2, 200000.0);
    std::shared_ptr<MarineEnvironment> environment;
    if (marine)
    {
        OGRSpatialReference geographic;
        geographic.importFromEPSG(4326);
        geographic.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        auto spatial = terrainSrs;
        spatial.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        auto* transform = OGRCreateCoordinateTransformation(&spatial, &geographic);
        double longitude = originX, latitude = originY;
        if (!transform)
            throw std::runtime_error("Cannot locate marine scene geographically");
        const bool transformed = transform->Transform(1, &longitude, &latitude) != 0;
        OCTDestroyCoordinateTransformation(transform);
        if (!transformed)
            throw std::runtime_error("Marine location transform failed");
        environment = std::make_shared<MarineEnvironment>(&viewer, scene, longitude, latitude);
        viewer.addEventHandler(new MarineKeys(environment, manipulator));
        surfaceView(manipulator);
    }
    viewer.setUpViewInWindow(100, 100, 1200, 800);
    osgViewer::Viewer::Windows windows;
    viewer.getWindows(windows);
    for (auto* window : windows)
        window->setWindowName(
            marine ? "Veylaxis - Region 001 - SilverLining sky + Triton ocean + seabed - XYZ 1:1"
                   : (scale == 1.0 ? "Veylaxis - Region 001 seabed - XYZ 1:1"
                                   : "Veylaxis - Region 001 seabed - Z x" + std::to_string(scale)));
    manipulator->home(0.0);
    std::cout
        << "Viewer controls: left drag orbit, middle pan, wheel zoom, Space reset, W wireframe; F1 surface, F2 underwater, O toggle ocean.\n"
        << std::flush;
    osg::ref_ptr<CaptureFrame> capture = new CaptureFrame;
    if (!smokePath.empty())
        viewer.getCamera()->setFinalDrawCallback(capture);
    viewer.realize();
    if (!smokePath.empty())
    {
        // Automated captures must not inherit mouse input from the visible
        // smoke-test window while the user continues working on the desktop.
        viewer.setCameraManipulator(nullptr);
        if (marine)
            viewer.getCamera()->setViewMatrixAsLookAt({350, -700, 60}, {0, 300, -5}, {0, 0, 1});
        else
            viewer.getCamera()->setViewMatrix(manipulator->getInverseMatrix());
    }
    const auto start = std::chrono::steady_clock::now();
    int smokePhase = 0, result = 0;
    bool announced = false, smokeDone = false;
    while (!viewer.done())
    {
        if (environment)
            environment->updateLighting();
        viewer.frame();
        if (environment && environment->failed())
        {
            std::cerr << "Marine SDK initialization failed; stopping scene safely\n";
            result = 3;
            break;
        }
        const bool ready = !environment || environment->ready();
        if (ready && !announced)
        {
            std::cout << "Environment ready: sky=" << (environment ? 1 : 0)
                      << " ocean=" << (environment ? 1 : 0) << " terrain=1 XYZ_scale=1\n"
                      << std::flush;
            announced = true;
        }
        const double elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        if (smokePath.empty())
            continue;
        if (ready && elapsed >= (smokePhase == 0 ? 6.0 : 10.0))
        {
            capture->requested = true;
            viewer.frame();
            std::filesystem::path path = smokePath;
            if (smokePhase == 1)
                path =
                    path.parent_path() / (path.stem().string() + "-underwater" + path.extension().string());
            if (!capture->image || !osgDB::writeImageFile(*capture->image, path.string()))
            {
                result = 3;
                break;
            }
            std::cout << "Marine preview: " << path.string() << '\n' << std::flush;
            osg::Vec3d eye, target, up;
            viewer.getCamera()->getViewMatrixAsLookAt(eye, target, up);
            std::cout << "Capture camera Z=" << eye.z() << " m; "
                      << (smokePhase == 0 ? "surface" : "underwater") << '\n'
                      << std::flush;
            if ((smokePhase == 0 && eye.z() <= 0) || (smokePhase == 1 && eye.z() >= 0))
            {
                result = 3;
                break;
            }
            if (smokePhase == 1 || !marine)
            {
                smokeDone = true;
                break;
            }
            viewer.getCamera()->setViewMatrixAsLookAt({-300, -250, -12}, {-20, 250, -20}, {0, 0, 1});
            ++smokePhase;
        }
        if (elapsed > 30.0)
        {
            std::cerr << "Marine SDK initialization timed out\n";
            result = 3;
            break;
        }
    }
    if (!smokePath.empty() && !smokeDone)
        result = 3;
    if (environment)
        environment->shutdown();
    viewer.setSceneData(nullptr);
    return result;
}
