// 【阅读导航】S-57 输入适配器：应用 .000 及连续更新文件，投影到配置的米制坐标，保留来源和原始属性。
#include "veylaxis/vgds/S57.hpp"
#include "veylaxis/vgds/Grid.hpp"
#include "BathymetryReader.hpp"
#include <ogrsf_frmts.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <memory>
#include <set>
#include <stdexcept>

namespace veylaxis::vgds
{
namespace
{
namespace fs = std::filesystem;
using J = nlohmann::json;

template <class T> T value(Result<T> r)
{
    if (auto e = std::get_if<TileError>(&r))
        throw std::runtime_error(e->message);
    return std::get<T>(std::move(r));
}

std::string field(OGRFeature& f, const char* name)
{
    int i = f.GetFieldIndex(name);
    return i >= 0 && f.IsFieldSetAndNotNull(i) ? f.GetFieldAsString(i) : "";
}

Ring ring(const OGRLinearRing& r)
{
    Ring out;
    for (int i = 0; i < r.getNumPoints() - 1; ++i)
        out.points.push_back({r.getX(i), r.getY(i)});
    out.edges.assign(out.points.size(), BoundaryKind::Unknown);
    return out;
}

Polygon polygon(const OGRPolygon& p)
{
    Polygon out;
    out.outer = ring(*p.getExteriorRing());
    for (int i = 0; i < p.getNumInteriorRings(); ++i)
        out.holes.push_back(ring(*p.getInteriorRing(i)));
    return out;
}

LineString line(const OGRLineString& l)
{
    LineString out;
    for (int i = 0; i < l.getNumPoints(); ++i)
        out.points.push_back({l.getX(i), l.getY(i)});
    out.edges.assign(out.points.size() - 1, BoundaryKind::Geographic);
    return out;
}

std::map<std::string, Property> attributes(OGRFeature& f)
{
    std::map<std::string, Property> out;
    for (int i = 0; i < f.GetFieldCount(); ++i)
    {
        auto* def = f.GetFieldDefnRef(i);
        std::string key = def->GetNameRef();
        if (!f.IsFieldSetAndNotNull(i))
        {
            out[key] = std::monostate{};
            continue;
        }
        switch (def->GetType())
        {
        case OFTInteger:
        case OFTInteger64:
            out[key] = static_cast<std::int64_t>(f.GetFieldAsInteger64(i));
            break;
        case OFTReal:
            out[key] = f.GetFieldAsDouble(i);
            break;
        case OFTStringList:
        {
            std::vector<std::string> values;
            for (char** p = f.GetFieldAsStringList(i); p && *p; ++p)
                values.emplace_back(*p);
            out[key] = std::move(values);
            break;
        }
        case OFTIntegerList:
        {
            std::vector<std::string> values;
            int n = 0;
            auto* a = f.GetFieldAsIntegerList(i, &n);
            for (int j = 0; j < n; ++j)
                values.push_back(std::to_string(a[j]));
            out[key] = std::move(values);
            out["s57_field_type." + key] = std::string("IntegerList");
            break;
        }
        default:
            out[key] = std::string(f.GetFieldAsString(i));
            out["s57_field_type." + key] = std::string(OGRFieldDefn::GetFieldTypeName(def->GetType()));
            break;
        }
    }
    return out;
}

Bounds bounds(const Geometry& geometry)
{
    Bounds b{INFINITY, INFINITY, -INFINITY, -INFINITY};
    auto add = [&](const Ring& r)
    {
        for (auto p : r.points)
        {
            b.minX = std::min(b.minX, p.x);
            b.minY = std::min(b.minY, p.y);
            b.maxX = std::max(b.maxX, p.x);
            b.maxY = std::max(b.maxY, p.y);
        }
    };
    if (auto p = std::get_if<Polygon>(&geometry))
        add(p->outer);
    else if (auto m = std::get_if<MultiPolygon>(&geometry))
        for (const auto& part : m->polygons)
            add(part.outer);
    return b;
}

// LNDARE outlines may also follow an ENC coverage edge. Only a COALNE-confirmed
// segment is treated as shoreline for wall generation; unconfirmed source edges
// stay Unknown. New tile cuts are classified separately by Geometry.cpp.
void classifyCoasts(Dataset& dataset, S57Report& report)
{
    struct Edge
    {
        XY a, b;
        double minX, maxX, minY, maxY;
    };

    std::vector<Edge> coast;
    for (const auto& f : dataset.features)
        if (f.layer == "coastline")
            if (auto l = std::get_if<LineString>(&f.geometry))
                for (std::size_t i = 1; i < l->points.size(); ++i)
                {
                    auto a = l->points[i - 1], b = l->points[i];
                    coast.push_back({a, b, std::min(a.x, b.x), std::max(a.x, b.x), std::min(a.y, b.y),
                                     std::max(a.y, b.y)});
                }
    // A small in-memory bucket index avoids a quadratic shoreline comparison.
    // Buckets use the configured grid, not a second hardcoded spatial grid.
    using Key = std::pair<std::int64_t, std::int64_t>;
    std::map<Key, std::vector<std::size_t>> buckets;
    std::vector<std::size_t> longEdges;
    for (std::size_t i = 0; i < coast.size(); ++i)
    {
        const auto& e = coast[i];
        auto lo = value(locate(dataset.grid, {e.minX, e.minY})),
             hi = value(locate(dataset.grid, {e.maxX, e.maxY}));
        if (static_cast<long double>(hi.x) - lo.x > 64 || static_cast<long double>(hi.y) - lo.y > 64)
        {
            longEdges.push_back(i);
            continue;
        }
        for (auto x = lo.x; x <= hi.x; ++x)
            for (auto y = lo.y; y <= hi.y; ++y)
                buckets[{x, y}].push_back(i);
    }
    auto on = [](XY p, const Edge& e)
    {
        double dx = e.b.x - e.a.x, dy = e.b.y - e.a.y, n = dx * dx + dy * dy;
        if (n == 0)
            return false;
        double t = ((p.x - e.a.x) * dx + (p.y - e.a.y) * dy) / n;
        return t >= -1e-9 && t <= 1 + 1e-9 &&
               std::abs((p.x - e.a.x) * dy - (p.y - e.a.y) * dx) <= 1e-7 * std::sqrt(n);
    };
    auto classify = [&](Ring& r)
    {
        for (std::size_t i = 0; i < r.points.size(); ++i)
        {
            auto a = r.points[i], b = r.points[(i + 1) % r.points.size()];
            XY midpoint{(a.x + b.x) / 2, (a.y + b.y) / 2};
            auto tile = value(locate(dataset.grid, midpoint));
            auto bucket = buckets.find({tile.x, tile.y});
            auto confirm = [&](const std::vector<std::size_t>& candidates)
            {
                for (auto index : candidates)
                {
                    const auto& e = coast[index];
                    if (on(a, e) && on(b, e))
                        return true;
                }
                return false;
            };
            if ((bucket != buckets.end() && confirm(bucket->second)) || confirm(longEdges))
                r.edges[i] = BoundaryKind::Geographic;
            if (r.edges[i] == BoundaryKind::Geographic)
                ++report.geographicEdges;
            else
                ++report.unknownEdges;
        }
    };
    for (auto& f : dataset.features)
        if (f.layer == "land")
        {
            auto polygon = [&](Polygon& p)
            {
                classify(p.outer);
                for (auto& h : p.holes)
                    classify(h);
                f.properties["boundary_classification"] =
                    std::string("COALNE-confirmed-v1; unconfirmed source edges Unknown");
            };
            if (auto p = std::get_if<Polygon>(&f.geometry))
                polygon(*p);
            else if (auto m = std::get_if<MultiPolygon>(&f.geometry))
                for (auto& part : m->polygons)
                    polygon(part);
        }
}
}
// 更新顺序、来源哈希和投影在这里统一处理；业务层不应再次猜测原始坐标或对象类别。
Result<Dataset> readS57(const std::string& path, const GridSpec& grid, S57Report* output,
                        S57Options readOptions)
try
{
    const auto start = std::chrono::steady_clock::now();
    if (auto e = validate(grid))
        return *e;
    GDALAllRegister();
    if (!OGRGeometryFactory::haveGEOS())
        throw std::runtime_error("GDAL lacks GEOS");
    fs::path base = fs::absolute(path);
    if (base.extension() != ".000")
        throw std::runtime_error("S57 base input must be .000");
    S57Report report;
    report.cell = base.stem().string();
    report.targetCrs = grid.horizontalCrs;
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(base.parent_path()))
    {
        auto p = e.path();
        auto ext = p.extension().string();
        if (p.stem() == base.stem() && ext.size() == 4 && ext[0] == '.' &&
            std::all_of(ext.begin() + 1, ext.end(),
                        [](char c)
                        {
                            return c >= '0' && c <= '9';
                        }))
            files.push_back(p);
    }
    std::sort(files.begin(), files.end());
    J manifest = J::array();
    int expected = 0;
    for (const auto& p : files)
    {
        if (std::stoi(p.extension().string().substr(1)) != expected++)
            throw std::runtime_error("S57 update sequence has a gap");
        report.sourceFiles.push_back(p.filename().string());
        manifest.push_back({{"file", p.filename().string()}, {"sha256", fileHash(p.string())}});
    }
    if (files.empty() || files.front() != base)
        throw std::runtime_error("S57 base file missing");
    const char* drivers[] = {"S57", nullptr};
    const char* options[] = {"UPDATES=APPLY", "LNAM_REFS=ON", nullptr};
    CPLErrorReset();
    std::unique_ptr<GDALDataset, decltype(&GDALClose)> source(
        static_cast<GDALDataset*>(
            GDALOpenEx(base.string().c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY, drivers, options, nullptr)),
        GDALClose);
    if (!source || CPLGetLastErrorType() >= CE_Failure)
        throw std::runtime_error("Cannot read S57 base/update files");
    auto* dsid = source->GetLayerByName("DSID");
    if (!dsid)
        throw std::runtime_error("Missing S57 DSID");
    std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)> identity(dsid->GetNextFeature(),
                                                                                OGRFeature::DestroyFeature);
    if (!identity)
        throw std::runtime_error("Missing S57 dataset identity");
    report.edition = field(*identity, "DSID_EDTN");
    report.update = field(*identity, "DSID_UPDN");
    report.soundingDatum = identity->GetFieldAsInteger("DSPM_SDAT");
    report.compilationScale = identity->GetFieldAsDouble("DSPM_CSCL");
    if (readOptions.bathymetry &&
        (identity->GetFieldAsInteger("DSPM_DUNI") != 1 || report.soundingDatum <= 0))
        throw std::runtime_error("Unknown sounding datum or non-metre depth units");
    if (report.edition.empty() || report.update.empty())
        throw std::runtime_error("Missing S57 edition/update");
    if (std::stoi(report.update) != static_cast<int>(files.size()) - 1)
        throw std::runtime_error("Applied DSID update does not match available update sequence");
    OGRSpatialReference target;
    if (target.SetFromUserInput(grid.horizontalCrs.c_str()) != OGRERR_NONE || !target.IsProjected() ||
        std::abs(target.GetLinearUnits() - 1) > 1e-9)
        throw std::runtime_error("S57 target CRS must be projected in metres");
    target.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    Dataset dataset;
    dataset.id = "s57:" + report.cell;
    dataset.grid = grid;
    dataset.provenance = Provenance::Observed;
    dataset.version = contentHash(J{
        {"files", manifest},
        {"edition", report.edition},
        {"update", report.update},
        {"gdal", GDALVersionInfo("RELEASE_NAME")},
        {"adapter", readOptions.maritime ? "s57-maritime-v3-model-classes" : "s57-land-v2-coast-confirmed"},
        {"bathy", readOptions.bathymetry},
        {"maritime",
         readOptions.maritime}}.dump());
    report.version = dataset.version;
    std::vector<std::string> names{"LNDARE", "COALNE"};
    if (readOptions.maritime)
    {
        dataset.supportedLayers.insert(dataset.supportedLayers.end(),
                                       {"fairway", "dredged_areas", "navigation_aids"});
        for (const char* name :
             {"FAIRWY", "DRGARE", "LIGHTS", "BOYLAT", "BOYCAR", "BOYISD", "BOYSAW", "BOYSPP", "BCNLAT",
              "BCNCAR", "BCNISD", "BCNSAW", "BCNSPP", "LITVES", "MORFAC", "BERTHS"})
            names.emplace_back(name);
    }
    for (const auto& layerNameString : names)
    {
        const char* layerName = layerNameString.c_str();
        auto* layer = source->GetLayerByName(layerName);
        if (!layer)
        {
            if (std::string(layerName) == "LNDARE")
                throw std::runtime_error("Missing LNDARE; check S57_CSV/GDAL_DATA");
            continue;
        }
        auto* sourceSrs = layer->GetSpatialRef();
        if (!sourceSrs)
            throw std::runtime_error("S57 layer has no CRS");
        auto geographic = *sourceSrs;
        geographic.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        char* wkt = nullptr;
        geographic.exportToWkt(&wkt);
        report.sourceCrs = wkt ? wkt : "";
        CPLFree(wkt);
        std::unique_ptr<OGRCoordinateTransformation, decltype(&OCTDestroyCoordinateTransformation)> transform(
            OGRCreateCoordinateTransformation(&geographic, &target), OCTDestroyCoordinateTransformation);
        if (!transform)
            throw std::runtime_error("Cannot construct S57 coordinate transformation");
        layer->ResetReading();
        CPLErrorReset();
        while (auto* raw = layer->GetNextFeature())
        {
            requestCheckpoint();
            std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)> feature(
                raw, OGRFeature::DestroyFeature);
            auto* original = feature->GetGeometryRef();
            if (!original || original->IsEmpty())
            {
                ++report.unsupportedGeometries;
                continue;
            }
            std::unique_ptr<OGRGeometry> projected(original->clone());
            if (projected->transform(transform.get()) != OGRERR_NONE)
                throw std::runtime_error("S57 projection failed");
            projected->flattenTo2D();
            Feature f;
            f.layer = layerNameString == "LNDARE"   ? "land"
                      : layerNameString == "COALNE" ? "coastline"
                      : layerNameString == "FAIRWY" ? "fairway"
                      : layerNameString == "DRGARE" ? "dredged_areas"
                                                    : "navigation_aids";
            f.objectType = layerName;
            f.provenance = Provenance::Observed;
            f.dataVersion = dataset.version;
            f.fragmentId = "source";
            f.properties = attributes(*feature);
            auto lnam = field(*feature, "LNAM");
            std::string identityKey = lnam;
            if (identityKey.empty())
            {
                auto rcid = field(*feature, "RCID");
                if (rcid.empty())
                    throw std::runtime_error("S57 feature lacks LNAM and RCID");
                identityKey = "edition:" + report.edition + ":RCID:" + rcid;
            }
            f.sourceObjectId = report.cell + ":" + layerName + ":" + identityKey;
            f.properties["source_id_kind"] = std::string(lnam.empty() ? "edition+RCID" : "LNAM");
            f.properties["source_fid"] = static_cast<std::int64_t>(feature->GetFID());
            f.properties["source_cell"] = report.cell;
            f.properties["source_files"] = report.sourceFiles;
            f.properties["source_edition"] = report.edition;
            f.properties["source_update"] = report.update;
            f.properties["updates_applied"] = true;
            f.properties["source_crs_wkt"] = report.sourceCrs;
            f.properties["target_crs"] = grid.horizontalCrs;
            f.properties["source_content_version"] = dataset.version;
            const auto type = wkbFlatten(projected->getGeometryType());
            bool supported = true;
            if (type == wkbPolygon)
            {
                f.geometry = polygon(*projected->toPolygon());
                if (f.layer == "land")
                    ++report.landPolygons;
            }
            else if (type == wkbMultiPolygon)
            {
                MultiPolygon m;
                for (const auto* p : *projected->toMultiPolygon())
                    m.polygons.push_back(polygon(*p));
                f.geometry = std::move(m);
                if (f.layer == "land")
                    ++report.landPolygons;
            }
            else if (type == wkbPoint)
            {
                auto* p = projected->toPoint();
                f.geometry = Point{{p->getX(), p->getY()}};
                if (f.layer == "land")
                    ++report.landPoints;
            }
            else if (type == wkbLineString)
            {
                f.geometry = line(*projected->toLineString());
                if (f.layer == "land")
                    ++report.landLines;
                if (f.layer == "coastline")
                    ++report.coastLines;
            }
            else
                supported = false;
            if (supported)
            {
                ++report.objectCounts[layerName];
                dataset.features.push_back(std::move(f));
            }
            else
                ++report.unsupportedGeometries;
        }
        if (CPLGetLastErrorType() >= CE_Failure)
            throw std::runtime_error("S57 layer decoding failed");
    }
    classifyCoasts(dataset, report);
    if (readOptions.bathymetry)
    {
        dataset.terrainSource = readBathymetry(*source, target, report);
        dataset.supportedLayers.push_back("terrain");
    }
    report.loadMilliseconds =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    if (output)
        *output = report;
    return dataset;
}
catch (const RequestCancelled&)
{
    throw;
}
catch (const std::exception& e)
{
    return TileError{ErrorCode::GenerationFailure, e.what()};
}

Result<LandTestArea> selectLandTestArea(const Dataset& dataset)
try
{
    struct Candidate
    {
        const Feature* feature;
        Bounds box;
        double distance;
    };

    std::vector<Candidate> candidates;
    for (const auto& f : dataset.features)
        if (f.layer == "land" &&
            (std::holds_alternative<Polygon>(f.geometry) || std::holds_alternative<MultiPolygon>(f.geometry)))
        {
            auto b = bounds(f.geometry);
            if (b.maxX - b.minX > dataset.grid.tileSizeMeters * 2 ||
                b.maxY - b.minY > dataset.grid.tileSizeMeters * 2 || polygonArea(f.geometry) < 1000)
                continue;
            candidates.push_back({&f, b,
                                  std::hypot((b.minX + b.maxX) * .5 - dataset.grid.origin.x,
                                             (b.minY + b.maxY) * .5 - dataset.grid.origin.y)});
        }
    std::sort(candidates.begin(), candidates.end(),
              [](const auto& a, const auto& b)
              {
                  return a.distance < b.distance;
              });
    for (const auto& candidate : candidates)
    {
        const auto b = candidate.box;
        XY center{(b.minX + b.maxX) / 2, (b.minY + b.maxY) / 2};
        auto tile = value(locate(dataset.grid, center));
        int nonempty = 0;
        for (int y = -1; y <= 1; ++y)
            for (int x = -1; x <= 1; ++x)
            {
                TileID t{tile.gridId, tile.x + x, tile.y + y};
                auto pieces = value(clipFeature(*candidate.feature, value(boundsOf(dataset.grid, t)), t));
                if (!pieces.empty() && polygonArea(pieces[0].geometry) > 1)
                    ++nonempty;
            }
        if (nonempty >= 2)
            return LandTestArea{center, candidate.feature->sourceObjectId, b, tile};
    }
    throw std::runtime_error("No small cross-tile LNDARE candidate found; choose an explicit test area");
}
catch (const std::exception& e)
{
    return TileError{ErrorCode::GenerationFailure, e.what()};
}
}
