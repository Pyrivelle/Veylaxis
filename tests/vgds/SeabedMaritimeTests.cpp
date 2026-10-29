#include "veylaxis/vgds/Bathymetry.hpp"
#include "veylaxis/vgds/S57.hpp"
#include "veylaxis/vgds/Codec.hpp"
#include "veylaxis/vgds/Grid.hpp"
#include <filesystem>
#include <chrono>
#include <iostream>
#include <cmath>
#include <stdexcept>
#include <nlohmann/json.hpp>
#include <fstream>

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
int runSeabedMaritimeTests()
try
{
    checks = 0;
    GridSpec grid{"synthetic-depth", "LOCAL:m", {0, 0}, 1000};
    SeabedInput input;
    input.verticalDatum = "synthetic-depth-zero";
    Ring ring{{{0, 0}, {2000, 0}, {2000, 1000}, {0, 1000}},
              std::vector<BoundaryKind>(4, BoundaryKind::Geographic)};
    input.areas.push_back({"area", Polygon{ring, {}}, 0, 20});
    input.contours = {{{0, 0}, {0, 1000}, 0}, {{2000, 0}, {2000, 1000}, 20}};
    input.soundings = {{{1000, 500}, 12, 0}};
    auto source = makeSeabedSource(input);
    source.provenance = Provenance::Synthetic;
    check(std::abs(*source.elevation({500, 500}) + 5) < 1e-6, "Adjacent contours produce linear slope");
    check(std::abs(*source.elevation({1000, 500}) + 12) < 1e-6,
          "SOUNDG residual constrains estimated height");
    check(!source.elevation({-1, 500}), "DEPARE limits coverage");
    auto left = value(sampleTerrain(grid, {grid.id, 0, 0}, 10, source)),
         right = value(sampleTerrain(grid, {grid.id, 1, 0}, 10, source));
    check(left.columns == 101 && left.rows == 101, "101x101 shared node layout");
    for (std::size_t row = 0; row < 101; ++row)
    {
        auto a = row * 101 + 100, b = row * 101;
        check(left.valid[a] == right.valid[b] && left.elevations[a] == right.elevations[b],
              "Shared height is identical");
        check(left.normals[a] == right.normals[b], "Global seabed normals agree");
    }
    auto empty = value(sampleTerrain(grid, {grid.id, 9, 9}, 10, source));
    check(empty.valid[0] == 0 && std::isnan(empty.elevations[0]), "Unknown seabed remains masked NaN");
    namespace fs = std::filesystem;
    const auto root = fs::path(VEYLAXIS_SOURCE_DIR);
    S57Report report;
    GridSpec real{"utm19-demo-v1", "EPSG:32619", {503500, 4856500}, 1000};
    auto data = value(
        readS57((root / "S57_DATA/ENC_ROOT/US2EC04M/US2EC04M.000").string(), real, &report, {true, true}));
    check(report.depthAreas > 0 && report.depthContours > 0 && report.soundings > 0,
          "Actual depth source layers parsed");
    check(report.objectCounts["LIGHTS"] > 0 && report.objectCounts["BOYLAT"] > 0 &&
              report.objectCounts["FAIRWY"] > 0,
          "Actual maritime classes parsed");
    check(data.terrainSource && data.terrainSource->verticalDatum.find("12") != std::string::npos,
          "Explicit sounding datum");
    auto a = value(sampleTerrain(real, {real.id, 0, 0}, 10, *data.terrainSource)),
         b = value(sampleTerrain(real, {real.id, 1, 0}, 10, *data.terrainSource));
    std::size_t valid = 0, matched = 0;
    for (auto v : a.valid)
        valid += v;
    check(valid > 100, "Real ENC produces constrained seabed nodes");
    for (std::size_t row = 0; row < 101; ++row)
    {
        auto i = row * 101 + 100, j = row * 101;
        check(a.valid[i] == b.valid[j], "Real seam coverage matches");
        if (a.valid[i])
        {
            ++matched;
            check(a.elevations[i] == b.elevations[j] && a.normals[i] == b.normals[j],
                  "Real shared elevation and normals match");
        }
    }
    check(matched > 0, "Real shared seam actually has valid samples");
    auto cache = fs::temp_directory_path() /
                 ("veylaxis-depth-" +
                  contentHash(std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))
                      .substr(0, 12));
    ServiceConfig config;
    config.cacheDirectory = cache.string();
    LocalService provider(data, config);
    auto p = value(provider.request({"terrain", {real.id, 0, 0}, {"terrain"}}));
    check(p.terrain && p.terrain->provenance == Provenance::Estimated && p.features.empty(),
          "Estimated seabed returned only when requested");
    auto roundtrip = value(decodeJson(value(encodeJson(p))));
    check(roundtrip.terrain->normals == p.terrain->normals, "Optional normals roundtrip");
    auto bad = p;
    bad.terrain->normals[0] = {0, 0, 2};
    check(std::holds_alternative<TileError>(encodeJson(bad)), "Invalid normal rejected");
    const Feature* marker = nullptr;
    for (const auto& f : data.features)
        if (f.layer == "navigation_aids" && std::holds_alternative<Point>(f.geometry))
        {
            marker = &f;
            break;
        }
    check(marker != nullptr, "Navigation point found");
    auto markerTile = value(locate(real, std::get<Point>(marker->geometry).position));
    auto nav = value(provider.request({"nav", markerTile, {"navigation_aids"}}));
    check(!nav.features.empty() && !nav.terrain, "Navigation layer selection");
    bool preserved = false;
    for (const auto& f : nav.features)
    {
        check(f.layer == "navigation_aids" && f.properties.count("LNAM") && f.properties.count("source_cell"),
              "Navigation identity and raw attributes");
        preserved |= f.sourceObjectId == marker->sourceObjectId;
    }
    check(preserved, "Requested point preserved with original source identity");
    auto unsupported = value(provider.request({"known", markerTile, {"dredged_areas"}}));
    check(unsupported.metadata.layers.at("dredged_areas") == LayerState::Empty,
          "Known absent DRGARE is Empty, not failure");
    fs::create_directories(root / "output/vgds");
    nlohmann::json result = {{"depth_areas", report.depthAreas},
                             {"contour_features", report.depthContours},
                             {"soundings", report.soundings},
                             {"rejected_records", report.rejectedDepthRecords},
                             {"valid_nodes", valid},
                             {"shared_valid_nodes", matched},
                             {"classes", report.objectCounts},
                             {"checks", checks}};
    std::ofstream out(root / "output/vgds/s5-s6-validation.json");
    out << result.dump(2);
    std::cout << "VGDS S5/S6: " << checks << " checks passed; valid=" << valid << " shared=" << matched
              << " SOUNDG=" << report.soundings << "\n";
    return 0;
}
catch (const std::exception& e)
{
    std::cerr << "Seabed/maritime test failed: " << e.what() << '\n';
    return 1;
}
