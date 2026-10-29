#include <gdal_priv.h>
#include <ogrsf_frmts.h>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

namespace
{
void check(bool b, const char* message)
{
    if (!b)
        throw std::runtime_error(message);
}
}
int runRasterCheck(int argc, char** argv)
try
{
    check(argc == 2, "Expected raster path");
    GDALAllRegister();
    auto close = [](GDALDataset* p)
    {
        if (p)
            GDALClose(p);
    };
    std::unique_ptr<GDALDataset, decltype(close)> ds(
        static_cast<GDALDataset*>(GDALOpen(argv[1], GA_ReadOnly)), close);
    check(bool(ds), "Cannot reopen raster");
    auto* srs = ds->GetSpatialRef();
    check(srs && srs->IsProjected(), "Missing projected CRS");
    check(std::string(srs->GetAuthorityCode(nullptr)) == "32619", "Incorrect EPSG code");
    double gt[6];
    check(ds->GetGeoTransform(gt) == CE_None && gt[1] == 5 && gt[5] == -5, "Incorrect geotransform");
    const int w = ds->GetRasterXSize(), h = ds->GetRasterYSize();
    check(w > 190 && w < 210 && h > 190 && h < 210, "Incorrect region dimensions");
    std::vector<float> z(static_cast<size_t>(w) * h);
    auto* band = ds->GetRasterBand(1);
    check(band->GetRasterDataType() == GDT_Float32, "Incorrect raster type");
    check(band->RasterIO(GF_Read, 0, 0, w, h, z.data(), w, h, GDT_Float32, 0, 0) == CE_None,
          "Raster read failed");
    const double noData = band->GetNoDataValue();
    size_t valid = 0;
    const auto folder = std::filesystem::path(argv[1]).parent_path();
    std::unique_ptr<GDALDataset, decltype(close)> qds(
        static_cast<GDALDataset*>(GDALOpen((folder / "region001_quality.tif").string().c_str(), GA_ReadOnly)),
        close);
    std::unique_ptr<GDALDataset, decltype(close)> rawds(
        static_cast<GDALDataset*>(
            GDALOpen((folder / "region001_elevation_raw.tif").string().c_str(), GA_ReadOnly)),
        close);
    check(bool(qds) && bool(rawds), "Missing quality/raw raster");
    std::vector<unsigned char> quality(z.size());
    std::vector<float> rawZ(z.size());
    check(qds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, w, h, quality.data(), w, h, GDT_Byte, 0, 0) ==
              CE_None,
          "Cannot read quality");
    check(rawds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, w, h, rawZ.data(), w, h, GDT_Float32, 0, 0) ==
              CE_None,
          "Cannot read raw raster");
    auto path = std::filesystem::path(argv[1]).parent_path() / "region001_sources.gpkg";
    std::unique_ptr<GDALDataset, decltype(close)> vectors(
        static_cast<GDALDataset*>(
            GDALOpenEx(path.string().c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr)),
        close);
    check(bool(vectors), "Missing source extraction");
    auto* areas = vectors->GetLayerByName("DEPARE_roi");
    check(areas, "Missing DEPARE extraction");
    auto* roiLayer = vectors->GetLayerByName("roi");
    check(roiLayer, "Missing ROI");
    std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)> roi(roiLayer->GetNextFeature(),
                                                                           OGRFeature::DestroyFeature);
    check(bool(roi), "Empty ROI");
    for (int r = 0; r < h; ++r)
        for (int c = 0; c < w; ++c)
        {
            size_t i = static_cast<size_t>(r) * w + c;
            OGRPoint p(gt[0] + (c + 0.5) * gt[1], gt[3] + (r + 0.5) * gt[5]);
            bool inside = roi->GetGeometryRef()->Intersects(&p);
            if (!inside)
            {
                check(z[i] == noData && quality[i] == 0, "Raster escaped exact ROI");
                continue;
            }
            check(z[i] != noData && std::isfinite(z[i]), "Hole in ROI");
            ++valid;
            check(quality[i] >= 1 && quality[i] <= 3, "Missing quality flag");
            if (c + 1 < w && z[i + 1] != noData)
                check(std::abs(z[i] - z[i + 1]) < 3, "Unresolved horizontal step");
            if (r + 1 < h && z[i + w] != noData)
                check(std::abs(z[i] - z[i + w]) < 3, "Unresolved vertical step");
            if (quality[i] == 3)
            {
                auto* conflicts = vectors->GetLayerByName("DEPARE_conflicts");
                check(conflicts, "Missing conflict evidence");
                conflicts->ResetReading();
                bool nearby = false;
                while (auto* f = conflicts->GetNextFeature())
                {
                    if (f->GetGeometryRef()->Distance(&p) < 100.0001)
                        nearby = true;
                    OGRFeature::DestroyFeature(f);
                }
                check(nearby, "Repair escaped declared transition strip");
                check(z[i] <= 0 && z[i] >= -91.4, "Invalid transition elevation");
            }
            areas->ResetReading();
            bool constrained = false;
            while (auto* raw = areas->GetNextFeature())
            {
                std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)> f(
                    raw, OGRFeature::DestroyFeature);
                if (f->GetGeometryRef()->Intersects(&p))
                {
                    const double lo = f->GetFieldAsDouble("depth_min") - 1e-4,
                                 hi = f->GetFieldAsDouble("depth_max") + 1e-4;
                    check(-rawZ[i] >= lo && -rawZ[i] <= hi, "Raw DEPARE bounds violated");
                    if (quality[i] != 3)
                        check(-z[i] >= lo && -z[i] <= hi, "DEPARE bounds violated outside repair strip");
                    constrained = true;
                    break;
                }
            }
            check(constrained, "No DEPARE coverage");
        }
    check(valid > 38000, "Insufficient coverage");
    std::cout << "Reopened GeoTIFF: " << w << 'x' << h << ", " << valid
              << " cells; CRS, ROI, raw DEPARE bounds, localized repair and continuity passed\n";
    return 0;
}
catch (const std::exception& e)
{
    std::cerr << e.what() << '\n';
    return 1;
}
