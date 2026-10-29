#include "veylaxis/vgds/Configuration.hpp"
#include "veylaxis/vgds/Grid.hpp"
#include "veylaxis/vgds/Codec.hpp"
#ifdef VEYLAXIS_HAS_OSGB
#include "veylaxis/vgds/OsgBuilder.hpp"
#include "veylaxis/streaming/TileManager.hpp"
#include <osgDB/ReadFile>
#endif
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <thread>
#include <stdexcept>

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

using Intervals = std::map<std::string, std::vector<std::pair<double, double>>>;

Intervals boundary(const TilePackage& package, double coordinate, bool vertical)
{
    Intervals out;
    auto ring = [&](const Feature& f, const Ring& r)
    {
        for (std::size_t i = 0; i < r.points.size(); ++i)
        {
            auto a = r.points[i], b = r.points[(i + 1) % r.points.size()];
            if (r.edges[i] != BoundaryKind::ArtificialClip)
                continue;
            if ((vertical ? a.x : a.y) == coordinate && (vertical ? b.x : b.y) == coordinate)
                out[f.sourceObjectId].emplace_back(std::min(vertical ? a.y : a.x, vertical ? b.y : b.x),
                                                   std::max(vertical ? a.y : a.x, vertical ? b.y : b.x));
        }
    };
    for (const auto& f : package.features)
        if (f.layer == "land")
        {
            auto polygon = [&](const Polygon& p)
            {
                ring(f, p.outer);
                for (const auto& h : p.holes)
                    ring(f, h);
            };
            if (auto p = std::get_if<Polygon>(&f.geometry))
                polygon(*p);
            else if (auto m = std::get_if<MultiPolygon>(&f.geometry))
                for (const auto& part : m->polygons)
                    polygon(part);
        }
    for (auto& entry : out)
        std::sort(entry.second.begin(), entry.second.end());
    return out;
}

void sameBoundary(const Intervals& a, const Intervals& b)
{
    check(a.size() == b.size(), "Adjacent seam source count");
    for (const auto& entry : a)
    {
        check(b.count(entry.first) == 1, "Adjacent seam same source");
        auto& other = b.at(entry.first);
        check(entry.second.size() == other.size(), "Adjacent seam intervals count");
        for (std::size_t i = 0; i < other.size(); ++i)
            check(std::abs(entry.second[i].first - other[i].first) < 1e-7 &&
                      std::abs(entry.second[i].second - other[i].second) < 1e-7,
                  "Adjacent seam geometric continuity");
    }
}
}
int runVgdsS57Tests()
try
{
    checks = 0;
    namespace fs = std::filesystem;
    using J = nlohmann::json;
    GridSpec grid{"utm19-demo-v1", "EPSG:32619", {503500, 4856500}, 1000};
    const fs::path original = fs::path(VEYLAXIS_SOURCE_DIR) / "S57_DATA/ENC_ROOT/US2EC04M/US2EC04M.000";
    const auto root =
        fs::temp_directory_path() /
        ("veylaxis-s4-" +
         contentHash(std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))
             .substr(0, 16));
    fs::create_directories(root / "source");
    const auto base = root / "source/US2EC04M.000", update = root / "source/US2EC04M.001";
    fs::copy_file(original, base);
    fs::copy_file(original.parent_path() / "US2EC04M.001", update);
    S57Report report;
    auto dataset = value(readS57(base.string(), grid, &report));
    check(report.landPolygons > 0 && report.coastLines > 0, "Actual LNDARE and COALNE present");
    check(report.landPolygons + report.landPoints + report.landLines == 590,
          "Actual mixed LNDARE geometry count");
    check(report.unsupportedGeometries == 0, "No source geometry silently dropped in this ENC");
    check(report.geographicEdges > 0, "Shoreline edges confirmed from independent COALNE layer");
    check(report.sourceFiles == std::vector<std::string>{"US2EC04M.000", "US2EC04M.001"},
          "Base and update provenance");
    check(report.edition == "0" && report.update == "1", "Update applied to DSID");
    check(report.sourceCrs.find("4326") != std::string::npos && report.targetCrs == grid.horizontalCrs,
          "Source CRS and projected target recorded");
    std::set<std::string> ids;
    for (const auto& f : dataset.features)
    {
        check(ids.insert(f.sourceObjectId).second, "Unique source identities");
        check(f.provenance == Provenance::Observed && f.dataVersion == dataset.version,
              "Observed source, content version");
        check(f.properties.count("LNAM") &&
                  f.sourceObjectId.find(std::get<std::string>(f.properties.at("LNAM"))) != std::string::npos,
              "Cell and LNAM identity");
        check(f.properties.count("RCID") && f.properties.count("RVER") &&
                  std::get<bool>(f.properties.at("updates_applied")),
              "S57 raw attributes and update status retained");
    }
    auto area = value(selectLandTestArea(dataset));
    const auto found = std::find_if(dataset.features.begin(), dataset.features.end(),
                                    [&](const auto& f)
                                    {
                                        return f.sourceObjectId == area.sourceObjectId;
                                    });
    check(found != dataset.features.end(), "Selected real source");
    const double sourceArea = polygonArea(found->geometry);
    ServiceConfig config;
    config.cacheDirectory = (root / "cache").string();
#ifdef VEYLAXIS_HAS_OSGB
    config.osgbEnabled = true;
    config.builder = buildOsgb;
    config.builderVersion = OsgBuilderVersion;
#endif
    LocalService service(std::move(dataset), config);
    // Prove queries do not reopen/parse the ENC: remove only our two copied inputs.
    fs::remove(base);
    fs::remove(update);
    check(!fs::exists(base) && fs::exists(original), "Test source removed, original untouched");
    std::map<std::pair<int, int>, TilePackage> tiles;
    double fragmentsArea = 0;
    int selectedTiles = 0;
    std::set<std::string> fragments;
    std::uint64_t artifactBytes = 0;
    const auto start = std::chrono::steady_clock::now();
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
        {
            TileRequest request{"s57:" + std::to_string(x) + "," + std::to_string(y),
                                {grid.id, area.centerTile.x + x, area.centerTile.y + y},
                                {"land", "coastline"}};
            auto package = value(service.request(request));
            check(!validate(package), "S57 tile validates");
            for (const auto& f : package.features)
            {
                check(f.properties.at("source_cell") == Property(std::string("US2EC04M")) &&
                          f.dataVersion == service.dataset().version,
                      "Fragment source metadata");
                if (f.sourceObjectId == area.sourceObjectId)
                {
                    fragmentsArea += polygonArea(f.geometry);
                    ++selectedTiles;
                    check(fragments.insert(f.fragmentId).second, "Real source distinct tile fragments");
                }
            }
            for (const auto& a : package.artifacts)
            {
                artifactBytes += a.bytes;
#ifdef VEYLAXIS_HAS_OSGB
                check(osgDB::readRefNodeFile(a.path).valid(), "Real LNDARE OSGB readable");
#endif
            }
            auto repeated = value(service.request(request));
            check(value(encodeJson(package)) == value(encodeJson(repeated)),
                  "Real cache response deterministic");
            tiles[{x, y}] = std::move(package);
        }
    const double generationMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    check(selectedTiles >= 2, "Real island crosses tiles");
    check(std::abs(fragmentsArea - sourceArea) < std::max(1e-4, sourceArea * 1e-9),
          "Real island area conservation");
    int shared = 0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
        {
            auto& package = tiles.at({x, y});
            if (x < 1)
            {
                auto a = boundary(package, package.metadata.bounds.maxX, true),
                     b = boundary(tiles.at({x + 1, y}), package.metadata.bounds.maxX, true);
                sameBoundary(a, b);
                shared += static_cast<int>(a.size());
            }
            if (y < 1)
            {
                auto a = boundary(package, package.metadata.bounds.maxY, false),
                     b = boundary(tiles.at({x, y + 1}), package.metadata.bounds.maxY, false);
                sameBoundary(a, b);
                shared += static_cast<int>(a.size());
            }
        }
    check(shared > 0, "Real common boundaries checked");
    check(service.stats().generations == 9 && service.stats().hits == 9,
          "One generation per real tile, repeated requests cached");
#ifdef VEYLAXIS_HAS_OSGB
    using namespace veylaxis::streaming;
    auto sharedService = std::make_shared<LocalService>(service.dataset(), config);
    auto client = std::make_shared<TileClient>(
        [sharedService](const std::string& r)
        {
            return sharedService->requestJson(r);
        });
    osg::ref_ptr<osg::Group> scene = new osg::Group;
    {
        TileManager manager(grid, scene, makeLoader(client));
        for (int step : {0, 1, 2, 0})
        {
            XY position{area.center.x + 1000 * step, area.center.y};
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            for (;;)
            {
                check(!manager.tick(position), "Unchanged S3 manager accepts S57 service");
                auto s = manager.stats();
                if (s.active == 9 && s.loading == 0)
                    break;
                if (std::chrono::steady_clock::now() > deadline)
                    throw std::runtime_error("S57 dynamic loading timeout");
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }
        check(manager.stats().unloaded >= 6 && manager.stats().failures == 0,
              "Actual land crosses two loading boundaries");
    }
    check(scene->getNumChildren() == 0, "Real scene nodes released");
#endif
    GridSpec invalid = grid;
    invalid.horizontalCrs = "EPSG:4326";
    check(std::holds_alternative<TileError>(readS57(original.string(), invalid)),
          "Reject geographic-degree grid");
    check(std::holds_alternative<TileError>(readS57((root / "missing.000").string(), grid)),
          "Missing source fails");
    fs::create_directories(fs::path(VEYLAXIS_SOURCE_DIR) / "output/vgds");
    J result = {
        {"stage", "S4"},
        {"cell", report.cell},
        {"edition", report.edition},
        {"update", report.update},
        {"version", report.version},
        {"source_files", report.sourceFiles},
        {"land_polygons", report.landPolygons},
        {"land_points", report.landPoints},
        {"land_lines", report.landLines},
        {"coastline", report.coastLines},
        {"unsupported", report.unsupportedGeometries},
        {"source_id", area.sourceObjectId},
        {"center", {area.center.x, area.center.y}},
        {"center_tile", {area.centerTile.x, area.centerTile.y}},
        {"bounds",
         {area.sourceBounds.minX, area.sourceBounds.minY, area.sourceBounds.maxX, area.sourceBounds.maxY}},
        {"source_area_m2", sourceArea},
        {"fragment_area_m2", fragmentsArea},
        {"area_error_m2", std::abs(sourceArea - fragmentsArea)},
        {"fragment_tiles", selectedTiles},
        {"shared_boundary_objects", shared},
        {"startup_ms", report.loadMilliseconds},
        {"nine_requests_and_hits_ms", generationMs},
        {"osgb_bytes", artifactBytes},
        {"checks", checks}};
    result["coast_confirmed_edges"] = report.geographicEdges;
    result["unknown_source_edges"] = report.unknownEdges;
#ifdef VEYLAXIS_HAS_OSGB
    result["osgb_enabled"] = true;
    const auto reportName = "s4-validation.json";
#else
    result["osgb_enabled"] = false;
    const auto reportName = "s4-validation-no-osgb.json";
#endif
    std::ofstream out(fs::path(VEYLAXIS_SOURCE_DIR) / "output/vgds" / reportName);
    out << result.dump(2);
    std::cout << "VGDS S4: " << checks << " checks passed; source=" << area.sourceObjectId
              << " tile=" << area.centerTile.x << "," << area.centerTile.y << " parts=" << selectedTiles
              << " area_error=" << std::setprecision(12) << std::abs(sourceArea - fragmentsArea)
              << " m2; startup_ms=" << report.loadMilliseconds << "\n";
    return 0;
}
catch (const std::exception& e)
{
    std::cerr << "VGDS S4 failed: " << e.what() << '\n';
    return 1;
}
