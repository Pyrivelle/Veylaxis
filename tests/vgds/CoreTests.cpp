#include "veylaxis/vgds/Types.hpp"
#include "veylaxis/vgds/Grid.hpp"
#include "veylaxis/vgds/ITileProvider.hpp"
#include "veylaxis/vgds/Codec.hpp"
#include <nlohmann/json.hpp>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace
{
using namespace veylaxis::vgds;
int checks = 0;

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

template <class T> bool error(const Result<T>& r)
{
    return std::holds_alternative<TileError>(r);
}

GridSpec grid()
{
    return {"test-utm", "EPSG:32619", {503500, 4856500}, 1000};
}

Ring square(double x, double y, double size, BoundaryKind kind)
{
    return {{{x, y}, {x + size, y}, {x + size, y + size}, {x, y + size}}, std::vector<BoundaryKind>(4, kind)};
}
}
int runVgdsTests()
try
{
    checks = 0;
    const auto g = grid();
    check(value(locate(g, g.origin)) == TileID{g.id, 0, 0}, "Origin ownership");
    check(value(locate(g, {504500, 4857500})) == TileID{g.id, 1, 1}, "Positive corner ownership");
    check(value(locate(g, {502500, 4855500})) == TileID{g.id, -1, -1}, "Negative corner ownership");
    check(value(locate(g, {std::nextafter(503500.0, -INFINITY), 4856500})) == TileID{g.id, -1, 0},
          "West nextafter must not truncate");
    check(value(locate(g, {std::nextafter(504500.0, -INFINITY), 4856500})) == TileID{g.id, 0, 0},
          "East interior must stay inside");
    GridSpec other{"other", "LOCAL:metres", {-13.5, 80.25}, 250};
    check(value(locate(other, {236.5, 330.25})) == TileID{"other", 1, 1}, "Grid configuration was hardcoded");
    for (int y = -5; y <= 5; ++y)
        for (int x = -5; x <= 5; ++x)
        {
            auto b = value(boundsOf(g, {g.id, x, y}));
            auto east = value(boundsOf(g, {g.id, x + 1, y}));
            check(b.maxX == east.minX, "Shared grid coordinates differ");
            check(value(locate(g, {b.minX + 1, b.minY + 1})) == TileID{g.id, x, y}, "Grid inverse mapping");
        }
    check(value(nodeCount(g, 10)) == 101 && value(nodeCount(g, 5)) == 201, "Sample-node formula");
    check(error(nodeCount(g, 3)) && error(nodeCount(g, 0)) && error(nodeCount(g, NAN)),
          "Invalid spacing accepted");
    check(error(locate(g, {INFINITY, 0})) && error(boundsOf(g, {"wrong", 0, 0})),
          "Invalid grid request accepted");
    check(error(locate(g, {1e300, 0})) && error(boundsOf(g, {g.id, INT64_MAX, 0})),
          "Index/spatial precision overflow accepted");
    auto badGrid = g;
    badGrid.tileSizeMeters = 0;
    check(validate(badGrid).has_value(), "Invalid grid size");
    badGrid = g;
    badGrid.horizontalUnit = "degree";
    check(validate(badGrid).has_value(), "Non-metre coordinate units accepted");
    check(value(normalizeLayers({"terrain", "land", "terrain"})) ==
              std::vector<std::string>({"land", "terrain"}),
          "Layer normalization");
    check(error(normalizeLayers({})) && error(normalizeLayers({"Land"})), "Malformed layers accepted");
    MockProvider provider(g);
    TileRequest request{"request-1",
                        {g.id, 0, 0},
                        {"terrain", "land", "coastline", "navigation_aids", "future_layer", "land"},
                        10};
    auto package = value(provider.request(request));
    auto requestRound = value(decodeRequestJson(value(encodeRequestJson(request))));
    check(requestRound.tile == request.tile && requestRound.layers.size() == 5 &&
              requestRound.requestId == request.requestId,
          "Request roundtrip/normalization");
    check(value(encodeJson(value(provider.request(requestRound)))) == value(encodeJson(package)),
          "Decoded request changed response");
    auto invalidRequest = request;
    invalidRequest.schemaVersion = 2;
    check(error(provider.request(invalidRequest)) && error(encodeRequestJson(invalidRequest)),
          "Request version was ignored");
    invalidRequest = request;
    invalidRequest.terrainSpacingMeters = NAN;
    check(error(provider.request(invalidRequest)) && error(encodeRequestJson(invalidRequest)),
          "Invalid request spacing serialized");
    auto requestWire = nlohmann::json::parse(value(encodeRequestJson(request)));
    requestWire["tile"]["x"] = 1.5;
    check(error(decodeRequestJson(requestWire.dump())), "Fractional request TileID accepted");
    check(error(decodeRequestJson("{broken")), "Malformed request accepted");
    check(package.metadata.layers.size() == 5 && package.metadata.layers.at("land") == LayerState::Empty,
          "Layer filtering/empty state");
    check(package.metadata.layers.at("navigation_aids") == LayerState::NoCoverage &&
              package.metadata.layers.at("future_layer") == LayerState::Unsupported,
          "Missing/unsupported states conflated");
    check(package.terrain && package.terrain->columns == 101 &&
              package.terrain->provenance == Provenance::Synthetic,
          "Mock is not explicitly synthetic");
    const auto encoded = value(encodeJson(package));
    check(encoded == value(encodeJson(value(provider.request(request)))),
          "Mock response is not deterministic");
    auto decoded = value(decodeJson(encoded));
    check(!validate(decoded) && decoded.metadata.requestId == request.requestId, "Roundtrip metadata");
    bool sameElevations = true;
    for (std::size_t i = 0; i < package.terrain->valid.size(); ++i)
        sameElevations &= package.terrain->valid[i]
                              ? decoded.terrain->elevations[i] == package.terrain->elevations[i]
                              : std::isnan(decoded.terrain->elevations[i]);
    check(decoded.terrain->valid == package.terrain->valid && sameElevations, "Terrain/masked NaN roundtrip");
    const auto center = decoded.terrain->valid.size() / 2;
    auto wire = nlohmann::json::parse(encoded);
    check(wire["terrain"]["elevations"][center].is_null() && !decoded.terrain->valid[center],
          "NoData became valid sea level");
    auto neighboringRequest = request;
    neighboringRequest.tile.x = 1;
    auto neighbor = value(provider.request(neighboringRequest));
    check(package.metadata.bounds.maxX == neighbor.metadata.bounds.minX, "Mock tile bounds mismatch");
    for (std::uint32_t row = 0; row < 101; ++row)
        check(package.terrain->elevations[row * 101 + 100] == neighbor.terrain->elevations[row * 101],
              "Mock boundary height mismatch");
    request.layers = {"land"};
    auto onlyLand = value(provider.request(request));
    check(!onlyLand.terrain && onlyLand.metadata.layers.size() == 1, "Unrequested payload returned");
    request.layers = {"terrain"};
    request.terrainSpacingMeters = 3;
    check(error(provider.request(request)), "Provider invalid resolution");
    // Structural geometry roundtrips, including mixed real/artificial edges and holes.
    auto p = onlyLand;
    p.metadata.layers["land"] = LayerState::Available;
    Polygon polygon{square(503600, 4856600, 200, BoundaryKind::Geographic),
                    {square(503650, 4856650, 20, BoundaryKind::Unknown)}};
    polygon.outer.edges[1] = BoundaryKind::ArtificialClip;
    Feature f;
    f.layer = "land";
    f.objectType = "MOCK_LAND";
    f.sourceObjectId = "mock:island-80";
    f.fragmentId = "mock:0:0:part-0";
    f.dataVersion = "fixture-1";
    f.geometry = polygon;
    f.properties = {{"name", std::string("模拟岛屿")},
                    {"largeId", INT64_MAX},
                    {"flag", true},
                    {"number", 1.25},
                    {"missing", std::monostate{}},
                    {"tags", std::vector<std::string>{"synthetic", "test"}}};
    p.features.push_back(f);
    for (const Geometry& geometry : std::vector<Geometry>{
             Point{{503700, 4856700}},
             LineString{{{503600, 4856600}, {504500, 4856700}}, {BoundaryKind::ArtificialClip}}, polygon,
             MultiPolygon{{polygon}}})
    {
        p.features[0].geometry = geometry;
        auto round = value(decodeJson(value(encodeJson(p))));
        check(value(encodeJson(round)) == value(encodeJson(p)),
              "Geometry/property/identity roundtrip changed semantics");
    }
    auto polygonRound = value(decodeJson(value(encodeJson(p))));
    const auto& multi = std::get<MultiPolygon>(polygonRound.features[0].geometry);
    check(multi.polygons[0].outer.edges[1] == BoundaryKind::ArtificialClip &&
              multi.polygons[0].holes.size() == 1,
          "Boundary roles or holes lost");
    auto badGeometry = nlohmann::json::parse(value(encodeJson(polygonRound)));
    badGeometry["features"][0]["geometry"]["polygons"][0]["outer"]["edges"][0] = 9;
    check(error(decodeJson(badGeometry.dump())), "Unknown boundary kind accepted");
    badGeometry = nlohmann::json::parse(value(encodeJson(polygonRound)));
    badGeometry["features"][0]["geometry"]["polygons"][0]["outer"]["edges"].erase(0);
    check(error(decodeJson(badGeometry.dump())), "Missing per-edge boundary classification accepted");
    p.features.push_back(p.features[0]);
    check(error(encodeJson(p)), "Duplicate fragment IDs accepted");
    p.features.pop_back();
    p.features[0].geometry = Point{{504500, 4856700}};
    check(error(encodeJson(p)), "Boundary point assigned twice");
    auto bad = wire;
    bad["schema"] = 2;
    check(error(decodeJson(bad.dump())), "Unknown schema accepted");
    bad = wire;
    bad["tile"]["x"] = 1.5;
    check(error(decodeJson(bad.dump())), "Fractional TileID accepted");
    bad = wire;
    bad["tile"]["x"] = 18446744073709551615ULL;
    check(error(decodeJson(bad.dump())), "Unsigned TileID overflow accepted");
    bad = wire;
    bad["terrain"]["valid"][0] = 2;
    check(error(decodeJson(bad.dump())), "Invalid mask accepted");
    bad = wire;
    bad["terrain"]["elevations"][0] = nullptr;
    check(error(decodeJson(bad.dump())), "Valid null accepted");
    bad = wire;
    bad["terrain"]["elevations"][center] = 0;
    check(error(decodeJson(bad.dump())), "Missing sample encoded as number");
    bad = wire;
    bad["terrain"]["elevations"][0] = 1e100;
    check(error(decodeJson(bad.dump())), "Float32 overflow accepted");
    bad = wire;
    bad["bounds"][0] = 0;
    check(error(decodeJson(bad.dump())), "Metadata/grid disagreement accepted");
    bad = wire;
    bad["layers"]["terrain"] = 99;
    check(error(decodeJson(bad.dump())), "Unknown layer state accepted");
    bad = wire;
    bad["terrain"]["columns"] = 1025;
    check(error(decodeJson(bad.dump())), "Oversized dimensions accepted");
    check(error(decodeJson("{}")) && error(decodeJson("{broken")), "Malformed JSON accepted");
    check(error(decodeJson("{\"schema\":1,\"schema\":1}")), "Duplicate JSON keys accepted");
    std::string nested = "0";
    for (int i = 0; i < 40; ++i)
        nested = "[" + nested + "]";
    check(error(decodeJson(nested)), "Excessive JSON nesting accepted");
    check(error(decodeJson(std::string(16 * 1024 * 1024 + 1, ' '))), "Oversized JSON accepted");
    auto invalid = package;
    invalid.terrain->elevations[0] = NAN;
    check(error(encodeJson(invalid)), "Valid NaN accepted");
    auto missing = package;
    missing.terrain.reset();
    check(error(encodeJson(missing)), "Missing available payload accepted");
    auto invalidSample = package;
    invalidSample.terrain->elevations[center] = NAN;
    check(!value(decodeJson(value(encodeJson(invalidSample)))).terrain->valid[center],
          "Invalid NaN was not normalized to masked null");
    auto noCoverage = package;
    std::fill(noCoverage.terrain->valid.begin(), noCoverage.terrain->valid.end(), 0);
    noCoverage.metadata.layers["terrain"] = LayerState::NoCoverage;
    const auto missingRoundtrip = value(decodeJson(value(encodeJson(noCoverage))));
    check(!validate(missingRoundtrip), "NoCoverage roundtrip");
    auto empty = onlyLand;
    check(!validate(empty), "Empty land is not a successful response");
    std::cout
        << "VGDS S1: " << checks
        << " checks passed (grid, boundaries, Mock, layers, geometry, JSON, NoData and malformed input)\n";
    return 0;
}
catch (const std::exception& e)
{
    std::cerr << "VGDS S1 failed: " << e.what() << '\n';
    return 1;
}
