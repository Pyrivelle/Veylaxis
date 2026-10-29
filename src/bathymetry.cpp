// 【阅读导航】离线 reconstruct 命令：读取指定 S-57 区域，调用海底算法并写带地理参考的 GeoTIFF。
#include "veylaxis/reconstruction.hpp"
#include <gdal_priv.h>
#include <gdal_alg.h>
#include <ogrsf_frmts.h>
#include <cpl_conv.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace fs = std::filesystem;
using namespace veylaxis;

namespace
{
struct CloseDataset
{
    void operator()(GDALDataset* p) const
    {
        if (p)
            GDALClose(p);
    }
};

using Dataset = std::unique_ptr<GDALDataset, CloseDataset>;
using Geometry = std::unique_ptr<OGRGeometry>;
using Feature = std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)>;

struct CloseTransform
{
    void operator()(OGRCoordinateTransformation* p) const
    {
        OCTDestroyCoordinateTransformation(p);
    }
};

using Transform = std::unique_ptr<OGRCoordinateTransformation, CloseTransform>;

struct Area
{
    Geometry polygon;
    DepthBand band;
};

void require(bool ok, const std::string& message)
{
    if (!ok)
        throw std::runtime_error(message);
}

struct Options
{
    std::string input = "S57_DATA/ENC_ROOT/US2EC04M/US2EC04M.000", output = "output/region001";
    double west = -68.956311, south = 43.861511, east = -68.944175, north = 43.870504;
    double resolution = 5, buffer = 20000, radius = 250;
    bool projectedBounds = false, gridNodes = false;
    double xmin{}, ymin{}, xmax{}, ymax{}, halo = 0;
};

Options parse(int argc, char** argv)
{
    Options o;
    auto value = [&](int& i)
    {
        require(i + 1 < argc, "Missing option value");
        return std::string(argv[++i]);
    };
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--input")
            o.input = value(i);
        else if (a == "--output")
            o.output = value(i);
        else if (a == "--resolution")
            o.resolution = std::stod(value(i));
        else if (a == "--buffer")
            o.buffer = std::stod(value(i));
        else if (a == "--sounding-radius")
            o.radius = std::stod(value(i));
        else if (a == "--projected-bounds")
        {
            o.projectedBounds = true;
            o.xmin = std::stod(value(i));
            o.ymin = std::stod(value(i));
            o.xmax = std::stod(value(i));
            o.ymax = std::stod(value(i));
        }
        else if (a == "--grid-nodes")
            o.gridNodes = true;
        else if (a == "--compute-halo")
            o.halo = std::stod(value(i));
        else if (a == "--algorithm")
            require(value(i) == "linear", "Supported algorithm: linear");
        else if (a == "--bounds")
        {
            o.west = std::stod(value(i));
            o.south = std::stod(value(i));
            o.east = std::stod(value(i));
            o.north = std::stod(value(i));
        }
        else if (a == "--help")
        {
            std::cout
                << "Veylaxis reconstruct [--input ENC.000] [--output DIR] [--resolution metres]\n"
                << "  [--bounds west south east north] [--buffer metres] [--sounding-radius metres] [--algorithm linear]\n"
                << "  [--projected-bounds xmin ymin xmax ymax] [--grid-nodes] [--compute-halo metres]\n"
                << "Region001 defaults; CRS EPSG:32619; elevations in metres below ENC sounding datum.\n";
            std::exit(0);
        }
        else
            throw std::runtime_error("Unknown argument: " + a);
    }
    require(std::isfinite(o.west) && std::isfinite(o.east) && std::isfinite(o.south) &&
                std::isfinite(o.north) && o.west < o.east && o.south < o.north,
            "Invalid bounds");
    require(o.west >= -72 && o.east <= -66 && o.south >= 0 && o.north <= 84,
            "Bounds must lie in northern UTM zone 19");
    require(std::isfinite(o.resolution) && o.resolution >= 0.5 && std::isfinite(o.buffer) && o.buffer >= 0 &&
                o.buffer <= 50000 && std::isfinite(o.radius) && o.radius > 0,
            "Invalid resolution/buffer/radius");
    require(std::isfinite(o.halo) && o.halo >= 0 && o.halo <= 2000 &&
                std::abs(o.halo / o.resolution - std::round(o.halo / o.resolution)) < 1e-8,
            "Halo must be an integer number of pixels, at most 2000m");
    if (o.projectedBounds)
        require(std::isfinite(o.xmin) && std::isfinite(o.ymin) && std::isfinite(o.xmax) &&
                    std::isfinite(o.ymax) && o.xmin < o.xmax && o.ymin < o.ymax && o.xmin >= 100000 &&
                    o.xmax <= 900000 && o.ymin >= 0 && o.ymax <= 9500000,
                "Invalid EPSG:32619 projected bounds");
    require(!o.gridNodes || o.projectedBounds, "Shared-node grids require explicit projected bounds");
    require(o.halo == 0 || o.projectedBounds, "Compute halo requires explicit projected bounds");
    if (o.gridNodes)
        require(std::abs((o.xmax - o.xmin) / o.resolution - std::round((o.xmax - o.xmin) / o.resolution)) <
                        1e-8 &&
                    std::abs((o.ymax - o.ymin) / o.resolution -
                             std::round((o.ymax - o.ymin) / o.resolution)) < 1e-8,
                "Node-grid bounds must be divisible by resolution");
    return o;
}

Geometry rectangle(double x0, double y0, double x1, double y1, int steps = 1)
{
    auto polygon = std::make_unique<OGRPolygon>();
    OGRLinearRing ring;
    const XY corners[] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}, {x0, y0}};
    for (int e = 0; e < 4; ++e)
        for (int i = 0; i < steps; ++i)
        {
            double t = static_cast<double>(i) / steps;
            ring.addPoint(corners[e].x * (1 - t) + corners[e + 1].x * t,
                          corners[e].y * (1 - t) + corners[e + 1].y * t);
        }
    ring.closeRings();
    polygon->addRing(&ring);
    return polygon;
}

void segments(const OGRGeometry* g, double depth, std::vector<Segment>& out)
{
    if (wkbFlatten(g->getGeometryType()) == wkbLineString)
    {
        const auto* l = g->toLineString();
        for (int i = 1; i < l->getNumPoints(); ++i)
            out.push_back({{l->getX(i - 1), l->getY(i - 1)}, {l->getX(i), l->getY(i)}, depth});
    }
    else if (OGR_GT_IsSubClassOf(g->getGeometryType(), wkbGeometryCollection))
    {
        const auto* c = g->toGeometryCollection();
        for (int i = 0; i < c->getNumGeometries(); ++i)
            segments(c->getGeometryRef(i), depth, out);
    }
}

OGRLayer* layer(GDALDataset* ds, const char* name, OGRSpatialReference& srs)
{
    auto* l = ds->CreateLayer(name, &srs, wkbUnknown, nullptr);
    require(l, "Cannot create extraction layer");
    for (const char* f : {"depth_min", "depth_max", "source_fid"})
    {
        OGRFieldDefn d(f, OFTReal);
        require(l->CreateField(&d) == OGRERR_NONE, "Cannot create field");
    }
    return l;
}

void record(OGRLayer* l, const OGRGeometry* g, double lo, double hi, GIntBig fid)
{
    if (!g || g->IsEmpty())
        return;
    Feature f(OGRFeature::CreateFeature(l->GetLayerDefn()), OGRFeature::DestroyFeature);
    f->SetField("depth_min", lo);
    f->SetField("depth_max", hi);
    f->SetField("source_fid", static_cast<double>(fid));
    require(f->SetGeometry(g) == OGRERR_NONE && l->CreateFeature(f.get()) == OGRERR_NONE,
            "Cannot write extraction feature");
}

int areaAt(const std::vector<Area>& areas, XY p)
{
    OGRPoint pt(p.x, p.y);
    for (size_t i = 0; i < areas.size(); ++i)
        if (areas[i].polygon->Intersects(&pt))
            return static_cast<int>(i);
    return -1;
}

Dataset raster(const fs::path& path, int w, int h, GDALDataType type, double* gt, OGRSpatialReference& srs)
{
    char* opts[] = {const_cast<char*>("COMPRESS=DEFLATE"), const_cast<char*>("TILED=YES"), nullptr};
    auto* driver = GetGDALDriverManager()->GetDriverByName("GTiff");
    require(driver, "GTiff driver unavailable");
    Dataset ds(driver->Create(path.string().c_str(), w, h, 1, type, opts));
    require(bool(ds), "Cannot create " + path.string());
    require(ds->SetGeoTransform(gt) == CE_None && ds->SetSpatialRef(&srs) == CE_None,
            "Cannot set raster CRS");
    ds->SetMetadataItem("AREA_OR_POINT", "Area");
    return ds;
}
}
int runReconstruction(int argc, char** argv)
try
{
    const auto o = parse(argc, argv);
    GDALAllRegister();
    require(OGRGeometryFactory::haveGEOS(), "GDAL requires GEOS for clipping and DEPARE constraints");
    const char* openOptions[] = {"SPLIT_MULTIPOINT=ON", "UPDATES=APPLY", nullptr};
    const char* drivers[] = {"S57", nullptr};
    Dataset source(static_cast<GDALDataset*>(
        GDALOpenEx(o.input.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY, drivers, openOptions, nullptr)));
    require(bool(source), "Cannot open S57 input: " + o.input);
    OGRSpatialReference geographic, projected;
    require(geographic.importFromEPSG(4326) == OGRERR_NONE && projected.importFromEPSG(32619) == OGRERR_NONE,
            "Cannot load EPSG definitions; check PROJ_DATA");
    geographic.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    projected.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    Transform forward(OGRCreateCoordinateTransformation(&geographic, &projected)),
        inverse(OGRCreateCoordinateTransformation(&projected, &geographic));
    require(bool(forward) && bool(inverse), "Cannot construct CRS transform");
    auto roi = o.projectedBounds ? rectangle(o.xmin, o.ymin, o.xmax, o.ymax)
                                 : rectangle(o.west, o.south, o.east, o.north, 64);
    if (!o.projectedBounds)
        require(roi->transform(forward.get()) == OGRERR_NONE, "ROI projection failed");
    Geometry outputRoi(roi->clone());
    OGREnvelope core;
    outputRoi->getEnvelope(&core);
    Geometry geographicRoi(outputRoi->clone());
    require(geographicRoi->transform(inverse.get()) == OGRERR_NONE, "Cannot locate output bounds");
    OGREnvelope geographicBounds;
    geographicRoi->getEnvelope(&geographicBounds);
    if (o.halo > 0)
        roi = rectangle(core.MinX - o.halo, core.MinY - o.halo, core.MaxX + o.halo, core.MaxY + o.halo);
    OGREnvelope env;
    roi->getEnvelope(&env);
    int width = static_cast<int>(std::ceil((env.MaxX - env.MinX) / o.resolution)) + (o.gridNodes ? 1 : 0),
        height = static_cast<int>(std::ceil((env.MaxY - env.MinY) / o.resolution)) + (o.gridNodes ? 1 : 0);
    require(width > 0 && height > 0 && static_cast<double>(width) * height <= 4000000,
            "Grid exceeds 4 million cells; increase resolution");
    double gt[] = {env.MinX - (o.gridNodes ? o.resolution / 2 : 0),
                   o.resolution,
                   0,
                   env.MaxY + (o.gridNodes ? o.resolution / 2 : 0),
                   0,
                   -o.resolution};
    auto context =
        rectangle(env.MinX - o.buffer, env.MinY - o.buffer, env.MaxX + o.buffer, env.MaxY + o.buffer, 32);
    auto geoContext = Geometry(context->clone());
    require(geoContext->transform(inverse.get()) == OGRERR_NONE, "Context inverse projection failed");
    OGREnvelope filter;
    geoContext->getEnvelope(&filter);
    fs::create_directories(o.output);
    const fs::path out = o.output;
    require(fs::absolute(o.input).parent_path() != fs::absolute(out),
            "Output must be separate from ENC source directory");
    auto gpkgPath = out / "region001_sources.gpkg";
    if (fs::exists(gpkgPath))
        require(GetGDALDriverManager()->GetDriverByName("GPKG")->Delete(gpkgPath.string().c_str()) == CE_None,
                "Cannot replace extraction GPKG");
    Dataset gpkg(GetGDALDriverManager()->GetDriverByName("GPKG")->Create(gpkgPath.string().c_str(), 0, 0, 0,
                                                                         GDT_Unknown, nullptr));
    require(bool(gpkg), "Cannot create source extraction");
    record(layer(gpkg.get(), "roi", projected), outputRoi.get(), 0, 0, 0);
    record(layer(gpkg.get(), "compute_roi", projected), roi.get(), 0, 0, 0);
    std::vector<Area> areas;
    ReconstructionInput model;
    model.soundingRadius = o.radius;
    int contourFeatures = 0, soundingsInRoi = 0, rejectedSoundings = 0, missingFields = 0;
    int verticalDatum = 0, soundingDatum = 0;
    double chartScale = 0;
    if (auto* l = source->GetLayerByName("DSID"))
    {
        Feature f(l->GetNextFeature(), OGRFeature::DestroyFeature);
        if (f)
        {
            int unit = f->GetFieldIndex("DSPM_DUNI");
            require(unit >= 0 && f->GetFieldAsInteger(unit) == 1,
                    "Only ENC depth units metres are supported");
            verticalDatum = f->GetFieldAsInteger("DSPM_VDAT");
            soundingDatum = f->GetFieldAsInteger("DSPM_SDAT");
            chartScale = f->GetFieldAsDouble("DSPM_CSCL");
        }
    }
    for (const char* name : {"DEPARE", "DEPCNT", "SOUNDG"})
    {
        auto* l = source->GetLayerByName(name);
        require(l, std::string("Missing layer ") + name);
        require(l->GetSpatialRef() && l->GetSpatialRef()->IsSame(&geographic),
                "Expected S57 WGS84 geographic coordinates");
        l->SetSpatialFilterRect(filter.MinX, filter.MinY, filter.MaxX, filter.MaxY);
        auto* support = layer(gpkg.get(), (std::string(name) + "_support").c_str(), projected);
        auto* clipped = layer(gpkg.get(), (std::string(name) + "_roi").c_str(), projected);
        while (auto* raw = l->GetNextFeature())
        {
            Feature f(raw, OGRFeature::DestroyFeature);
            const auto* original = f->GetGeometryRef();
            if (!original || original->IsEmpty())
                continue;
            Geometry g(original->clone());
            require(g->transform(forward.get()) == OGRERR_NONE, "Source projection failed");
            g = Geometry(g->Intersection(context.get()));
            require(bool(g), "Source clipping failed");
            if (g->IsEmpty())
                continue;
            double lo = 0, hi = 0;
            if (std::string(name) == "DEPARE")
            {
                if (!f->IsFieldSetAndNotNull(f->GetFieldIndex("DRVAL1")) ||
                    !f->IsFieldSetAndNotNull(f->GetFieldIndex("DRVAL2")))
                {
                    ++missingFields;
                    continue;
                }
                lo = f->GetFieldAsDouble("DRVAL1");
                hi = f->GetFieldAsDouble("DRVAL2");
                if (!std::isfinite(lo) || !std::isfinite(hi) || lo > hi)
                {
                    ++missingFields;
                    continue;
                }
                areas.push_back({Geometry(g->clone()), {lo, hi}});
                model.bands.push_back({lo, hi});
            }
            else if (std::string(name) == "DEPCNT")
            {
                if (!f->IsFieldSetAndNotNull(f->GetFieldIndex("VALDCO")))
                {
                    ++missingFields;
                    continue;
                }
                lo = hi = f->GetFieldAsDouble("VALDCO");
                if (!std::isfinite(lo))
                {
                    ++missingFields;
                    continue;
                }
                segments(g.get(), lo, model.contours);
                ++contourFeatures;
            }
            else
            {
                require(wkbFlatten(g->getGeometryType()) == wkbPoint && g->Is3D(),
                        "Expected split SOUNDG Point Z");
                const auto* p = g->toPoint();
                lo = hi = p->getZ();
                int a = areaAt(areas, {p->getX(), p->getY()});
                if (a >= 0 && std::isfinite(lo) && lo >= areas[a].band.lower && lo <= areas[a].band.upper)
                {
                    model.soundings.push_back({{p->getX(), p->getY()}, lo, a});
                    if (outputRoi->Intersects(g.get()))
                        ++soundingsInRoi;
                }
                else
                    ++rejectedSoundings;
            }
            record(support, g.get(), lo, hi, f->GetFID());
            Geometry inside(g->Intersection(outputRoi.get()));
            require(bool(inside), "ROI source clipping failed");
            record(clipped, inside.get(), lo, hi, f->GetFID());
        }
    }
    std::vector<Segment> conflictSegments;
    double conflictLength = 0;
    auto* conflictLayer = layer(gpkg.get(), "DEPARE_conflicts", projected);
    for (size_t a = 0; a < areas.size(); ++a)
        for (size_t b = a + 1; b < areas.size(); ++b)
        {
            const double lo = std::min(areas[a].band.upper, areas[b].band.upper),
                         hi = std::max(areas[a].band.lower, areas[b].band.lower);
            if (hi <= lo + 1e-5)
                continue;
            Geometry first(areas[a].polygon->Boundary()), second(areas[b].polygon->Boundary());
            Geometry common(first->Intersection(second.get()));
            require(bool(common), "DEPARE topology intersection failed");
            if (common->IsEmpty())
                continue;
            common = Geometry(common->Intersection(roi.get()));
            require(bool(common), "Conflict clipping failed");
            std::vector<Segment> edges;
            segments(common.get(), 0, edges);
            double length = 0;
            for (auto& s : edges)
                length += std::hypot(s.b.x - s.a.x, s.b.y - s.a.y);
            if (length < 1e-5)
                continue;
            record(conflictLayer, common.get(), lo, hi, static_cast<GIntBig>(a));
            conflictSegments.insert(conflictSegments.end(), edges.begin(), edges.end());
            conflictLength += length;
        }
    gpkg.reset();
    require(!areas.empty() && !model.contours.empty(), "No usable DEPARE/DEPCNT context");
    // Limit each DEPARE to its actual contour boundary, not an unrelated basin
    // with the same depth value. The midpoint tolerance absorbs reprojection roundoff.
    model.areaContourIndices.resize(areas.size());
    for (size_t a = 0; a < areas.size(); ++a)
    {
        Geometry boundary(areas[a].polygon->Boundary());
        require(bool(boundary), "Cannot get DEPARE boundary");
        for (size_t j = 0; j < model.contours.size(); ++j)
        {
            const auto& s = model.contours[j];
            const auto b = areas[a].band;
            if (std::abs(s.depth - b.lower) > 1e-5 && std::abs(s.depth - b.upper) > 1e-5)
                continue;
            OGRPoint midpoint((s.a.x + s.b.x) * 0.5, (s.a.y + s.b.y) * 0.5);
            if (boundary->Distance(&midpoint) < 0.05)
                model.areaContourIndices[a].push_back(j);
        }
    }
    std::cout << "Context: " << areas.size() << " DEPARE, " << contourFeatures << " DEPCNT features, "
              << model.contours.size() << " segments, " << model.soundings.size() << " SOUNDG (ROI "
              << soundingsInRoi << ")\n";
    // Replace this factory selection to add another ISeabedReconstructor implementation.
    std::unique_ptr<ISeabedReconstructor> algorithm = std::make_unique<LinearContourReconstructor>(model);
    // GDAL rasterization uses pixel centres (ALL_TOUCHED defaults to false).
    auto* mem = GetGDALDriverManager()->GetDriverByName("MEM");
    Dataset mask(mem->Create("", width, height, 1, GDT_Int32, nullptr));
    require(bool(mask), "Cannot create mask");
    mask->SetGeoTransform(gt);
    mask->SetSpatialRef(&projected);
    const int bandIndex = 1;
    OGRGeometryH roiHandle = OGRGeometry::ToHandle(roi.get());
    double burn = 1;
    require(GDALRasterizeGeometries(GDALDataset::ToHandle(mask.get()), 1, &bandIndex, 1, &roiHandle, nullptr,
                                    nullptr, &burn, nullptr, nullptr, nullptr) == CE_None,
            "Cannot rasterize ROI");
    const size_t n = static_cast<size_t>(width) * height;
    std::vector<int> roiMask(n), areaMask(n);
    require(mask->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, width, height, roiMask.data(), width, height,
                                             GDT_Int32, 0, 0) == CE_None,
            "Cannot read ROI mask");
    if (o.gridNodes)
        std::fill(roiMask.begin(), roiMask.end(), 1); // Include all four exact shared edges.
    mask->GetRasterBand(1)->Fill(0);
    // Reverse burning makes first source polygon win, matching areaAt().
    for (int a = static_cast<int>(areas.size()) - 1; a >= 0; --a)
    {
        OGRGeometryH geometry = OGRGeometry::ToHandle(areas[a].polygon.get());
        double id = a + 1;
        require(GDALRasterizeGeometries(GDALDataset::ToHandle(mask.get()), 1, &bandIndex, 1, &geometry,
                                        nullptr, nullptr, &id, nullptr, nullptr, nullptr) == CE_None,
                "Cannot rasterize DEPARE");
    }
    require(mask->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, width, height, areaMask.data(), width, height,
                                             GDT_Int32, 0, 0) == CE_None,
            "Cannot read DEPARE mask");
    constexpr float nodata = -9999;
    std::vector<float> elevations(n, nodata);
    std::vector<unsigned char> quality(n, 0);
    int valid = 0, fallback = 0, roiCells = 0, unsupported = 0;
    double minElevation = 0, maxElevation = -1e30;
    for (int row = 0; row < height; ++row)
        for (int col = 0; col < width; ++col)
        {
            const size_t i = static_cast<size_t>(row) * width + col;
            if (!roiMask[i])
                continue;
            ++roiCells;
            if (areaMask[i] == 0)
            {
                ++unsupported;
                continue;
            }
            XY p{gt[0] + (col + 0.5) * gt[1], gt[3] + (row + 0.5) * gt[5]};
            auto estimate = algorithm->evaluate(p, areaMask[i] - 1);
            if (!estimate)
            {
                ++unsupported;
                continue;
            }
            elevations[i] = static_cast<float>(-estimate->depth);
            quality[i] = static_cast<unsigned char>(estimate->quality + 1);
            ++valid;
            if (estimate->quality)
                ++fallback;
            minElevation = std::min(minElevation, static_cast<double>(elevations[i]));
            maxElevation = std::max(maxElevation, static_cast<double>(elevations[i]));
        }
    require(valid > 0, "No valid terrain cells");
    auto rawValues = elevations;
    auto rawQuality = quality;
    int repaired = repairDepthAreaConflicts(elevations, quality, width, height, {gt[0], gt[3]}, o.resolution,
                                            conflictSegments);
    std::vector<float> normalX, normalY, normalZ;
    if (o.gridNodes && o.halo >= o.resolution)
    {
        normalX.resize(elevations.size());
        normalY.resize(elevations.size());
        normalZ.resize(elevations.size());
        for (int row = 0; row < height; ++row)
            for (int col = 0; col < width; ++col)
            {
                const size_t i = static_cast<size_t>(row) * width + col;
                if (!quality[i])
                    continue;
                auto derivative = [&](int before, int after)
                {
                    const bool a = before >= 0 && quality[static_cast<size_t>(before)],
                               b = after >= 0 && quality[static_cast<size_t>(after)];
                    if (a && b)
                        return (elevations[after] - elevations[before]) / (2 * o.resolution);
                    if (a)
                        return (elevations[i] - elevations[before]) / o.resolution;
                    if (b)
                        return (elevations[after] - elevations[i]) / o.resolution;
                    return 0.0;
                };
                const double gx = derivative(col ? static_cast<int>(i) - 1 : -1,
                                             col + 1 < width ? static_cast<int>(i) + 1 : -1);
                const double gy = -derivative(row ? static_cast<int>(i) - width : -1,
                                              row + 1 < height ? static_cast<int>(i) + width : -1);
                const double length = std::sqrt(gx * gx + gy * gy + 1);
                normalX[i] = static_cast<float>(-gx / length);
                normalY[i] = static_cast<float>(-gy / length);
                normalZ[i] = static_cast<float>(1 / length);
            }
    }
    const int trim = static_cast<int>(std::round(o.halo / o.resolution));
    if (trim > 0)
    {
        const int croppedWidth = width - 2 * trim, croppedHeight = height - 2 * trim;
        auto crop = [&](auto& values)
        {
            using Values = std::decay_t<decltype(values)>;
            Values result;
            result.reserve(static_cast<size_t>(croppedWidth) * croppedHeight);
            for (int row = trim; row < height - trim; ++row)
                result.insert(result.end(), values.begin() + static_cast<size_t>(row) * width + trim,
                              values.begin() + static_cast<size_t>(row) * width + width - trim);
            values = std::move(result);
        };
        crop(elevations);
        crop(rawValues);
        crop(quality);
        crop(rawQuality);
        crop(roiMask);
        if (!normalX.empty())
        {
            crop(normalX);
            crop(normalY);
            crop(normalZ);
        }
        width = croppedWidth;
        height = croppedHeight;
        gt[0] += trim * o.resolution;
        gt[3] -= trim * o.resolution;
    }
    valid = 0;
    fallback = 0;
    repaired = 0;
    roiCells = 0;
    unsupported = 0;
    for (size_t i = 0; i < quality.size(); ++i)
        if (roiMask[i])
        {
            ++roiCells;
            if (quality[i])
                ++valid;
            else
                ++unsupported;
            if (rawQuality[i] == 2)
                ++fallback;
            if (quality[i] == 3)
                ++repaired;
        }
    auto rawElevation =
        raster(out / "region001_elevation_raw.tif", width, height, GDT_Float32, gt, projected);
    if (o.gridNodes)
        rawElevation->SetMetadataItem("AREA_OR_POINT", "Point");
    auto* rawBand = rawElevation->GetRasterBand(1);
    rawBand->SetNoDataValue(nodata);
    rawBand->SetUnitType("m");
    rawBand->SetDescription("DEPARE-constrained reconstruction before topology conflict repair");
    rawElevation->SetMetadataItem("VERTICAL_REFERENCE", "ENC sounding datum; no geoid/ellipsoid conversion");
    rawElevation->SetMetadataItem("ENC_DSPM_SDAT", std::to_string(soundingDatum).c_str());
    require(rawBand->RasterIO(GF_Write, 0, 0, width, height, rawValues.data(), width, height, GDT_Float32, 0,
                              0) == CE_None,
            "Cannot write raw elevation");
    require(rawElevation->FlushCache() == CE_None, "Cannot flush raw elevation");
    rawElevation.reset();
    double maxStep = 0;
    minElevation = 0;
    maxElevation = -1e30;
    for (int r = 0; r < height; ++r)
        for (int c = 0; c < width; ++c)
        {
            size_t i = static_cast<size_t>(r) * width + c;
            if (!quality[i])
                continue;
            minElevation = std::min(minElevation, static_cast<double>(elevations[i]));
            maxElevation = std::max(maxElevation, static_cast<double>(elevations[i]));
            if (c + 1 < width && quality[i + 1])
                maxStep = std::max(maxStep, static_cast<double>(std::abs(elevations[i] - elevations[i + 1])));
            if (r + 1 < height && quality[i + width])
                maxStep =
                    std::max(maxStep, static_cast<double>(std::abs(elevations[i] - elevations[i + width])));
        }
    auto elevation = raster(out / "region001_elevation.tif", width, height, GDT_Float32, gt, projected);
    if (o.gridNodes)
        elevation->SetMetadataItem("AREA_OR_POINT", "Point");
    if (!normalX.empty())
    {
        char* opts[] = {const_cast<char*>("COMPRESS=DEFLATE"), const_cast<char*>("TILED=YES"), nullptr};
        Dataset normals(GetGDALDriverManager()->GetDriverByName("GTiff")->Create(
            (out / "region001_normals.tif").string().c_str(), width, height, 3, GDT_Float32, opts));
        require(bool(normals), "Cannot create tile normal raster");
        normals->SetGeoTransform(gt);
        normals->SetSpatialRef(&projected);
        normals->SetMetadataItem("AREA_OR_POINT", "Point");
        std::vector<float>* data[] = {&normalX, &normalY, &normalZ};
        const char* names[] = {"Nx", "Ny", "Nz"};
        for (int b = 0; b < 3; ++b)
        {
            normals->GetRasterBand(b + 1)->SetDescription(names[b]);
            require(normals->GetRasterBand(b + 1)->RasterIO(GF_Write, 0, 0, width, height, data[b]->data(),
                                                            width, height, GDT_Float32, 0, 0) == CE_None,
                    "Cannot write halo-derived normals");
        }
        require(normals->FlushCache() == CE_None, "Cannot flush normals");
        elevation->SetMetadataItem("NORMALS_FILE", "region001_normals.tif");
    }
    elevation->SetMetadataItem("ALGORITHM",
                               "linear_contour_distance_with_sounding_residual_and_conflict_transition");
    elevation->SetMetadataItem("DEPARE_CONFLICT_REPAIR",
                               "100m radius; 15m full-blend core; quality=3; local depth bounds relaxed");
    elevation->SetMetadataItem("VERTICAL_REFERENCE", "ENC sounding datum; no geoid/ellipsoid conversion");
    elevation->SetMetadataItem("ENC_DSPM_VDAT", std::to_string(verticalDatum).c_str());
    elevation->SetMetadataItem("ENC_DSPM_SDAT", std::to_string(soundingDatum).c_str());
    elevation->SetMetadataItem("ENC_COMPILATION_SCALE", std::to_string(chartScale).c_str());
    elevation->SetMetadataItem("SOURCE_ENC", fs::absolute(o.input).string().c_str());
    auto* band = elevation->GetRasterBand(1);
    band->SetDescription("Seabed elevation (negative depth)");
    band->SetUnitType("m");
    band->SetNoDataValue(nodata);
    require(band->RasterIO(GF_Write, 0, 0, width, height, elevations.data(), width, height, GDT_Float32, 0,
                           0) == CE_None,
            "Cannot write elevation");
    require(elevation->FlushCache() == CE_None, "Cannot flush elevation");
    elevation.reset();
    auto q = raster(out / "region001_quality.tif", width, height, GDT_Byte, gt, projected);
    if (o.gridNodes)
        q->SetMetadataItem("AREA_OR_POINT", "Point");
    q->GetRasterBand(1)->SetNoDataValue(0);
    q->GetRasterBand(1)->SetDescription(
        "0=NoData; 1=two bounding contours; 2=one contour fallback; 3=DEPARE conflict transition");
    require(q->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, width, height, quality.data(), width, height,
                                          GDT_Byte, 0, 0) == CE_None,
            "Cannot write quality");
    require(q->FlushCache() == CE_None, "Cannot flush quality");
    q.reset();
    std::ofstream report(out / "region001_report.json");
    report
        << std::setprecision(12)
        << "{\n  \"algorithm\": \"linear_contour_distance_with_sounding_residual_and_conflict_transition\",\n  \"crs\": \"EPSG:32619\",\n"
        << "  \"bounds_wgs84\": [" << geographicBounds.MinX << ", " << geographicBounds.MinY << ", "
        << geographicBounds.MaxX << ", " << geographicBounds.MaxY << "],\n"
        << "  \"bounds_utm\": [" << core.MinX << ", " << core.MinY << ", " << core.MaxX << ", " << core.MaxY
        << "],\n"
        << "  \"grid_nodes\": " << (o.gridNodes ? "true" : "false") << ",\n  \"compute_halo_m\": " << o.halo
        << ",\n"
        << "  \"roi_area_m2\": " << outputRoi->toPolygon()->get_Area()
        << ",\n  \"resolution_m\": " << o.resolution << ",\n"
        << "  \"width\": " << width << ", \"height\": " << height << ",\n  \"buffer_m\": " << o.buffer
        << ",\n  \"sounding_radius_m\": " << o.radius << ",\n"
        << "  \"context_depare_features\": " << areas.size()
        << ",\n  \"context_depcnt_features\": " << contourFeatures
        << ",\n  \"context_contour_segments\": " << model.contours.size() << ",\n"
        << "  \"context_usable_soundings\": " << model.soundings.size()
        << ",\n  \"roi_usable_soundings\": " << soundingsInRoi
        << ",\n  \"rejected_soundings\": " << rejectedSoundings
        << ",\n  \"missing_or_invalid_fields\": " << missingFields << ",\n"
        << "  \"roi_cells\": " << roiCells << ",\n  \"valid_cells\": " << valid
        << ",\n  \"one_contour_fallback_cells\": " << fallback
        << ",\n  \"unsupported_roi_cells\": " << unsupported << ",\n"
        << "  \"conflict_boundary_length_m\": " << conflictLength
        << ",\n  \"conflict_transition_cells\": " << repaired
        << ",\n  \"conflict_transition_radius_m\": 100,\n  \"max_adjacent_cell_step_m\": " << maxStep << ",\n"
        << "  \"minimum_elevation_m\": " << minElevation << ",\n  \"maximum_elevation_m\": " << maxElevation
        << ",\n  \"enc_dspm_vdat\": " << verticalDatum << ",\n  \"enc_dspm_sdat\": " << soundingDatum
        << ",\n  \"enc_compilation_scale\": " << chartScale << "\n}\n";
    require(bool(report), "Cannot write report");
    std::ofstream csv(out / "region001_sounding_check.csv");
    csv << std::setprecision(12) << "easting,northing,observed_depth_m,reconstructed_depth_m,in_roi\n";
    for (const auto& s : model.soundings)
    {
        auto e = algorithm->evaluate(s.position, s.area);
        OGRPoint p(s.position.x, s.position.y);
        csv << s.position.x << ',' << s.position.y << ',' << s.depth << ',' << (e ? e->depth : nodata) << ','
            << (outputRoi->Intersects(&p) ? 1 : 0) << '\n';
    }
    require(bool(csv), "Cannot write sounding check");
    std::cout << width << " x " << height << " at " << o.resolution << " m; valid " << valid << "/"
              << roiCells << "; one-contour fallback " << fallback << "; unsupported " << unsupported << "\n"
              << "DEPARE conflict boundary " << conflictLength << " m; transition cells " << repaired
              << "; max adjacent step " << maxStep << " m\n"
              << "Elevation " << minElevation << " .. " << maxElevation << " m; output "
              << fs::absolute(out).string() << '\n';
    // Partial coverage is explicit and produces a failing exit status for automation.
    return unsupported ? 2 : 0;
}
catch (const std::exception& e)
{
    std::cerr << "Error: " << e.what() << '\n';
    return 1;
}
