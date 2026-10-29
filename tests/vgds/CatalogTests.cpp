#include "veylaxis/vgds/Catalog.hpp"
#include "veylaxis/vgds/Codec.hpp"
#include "veylaxis/vgds/Grid.hpp"
#include <filesystem>
#include <iostream>
#include <chrono>
#include <stdexcept>
#include <fstream>
#include <nlohmann/json.hpp>

namespace
{
using namespace veylaxis::vgds;
int checks;

void check(bool v, const char* text)
{
    ++checks;
    if (!v)
        throw std::runtime_error(text);
}

template <class T> T value(Result<T> r)
{
    if (auto e = std::get_if<TileError>(&r))
        throw std::runtime_error(e->message);
    return std::get<T>(std::move(r));
}
}
int runCatalogTests()
try
{
    namespace fs = std::filesystem;
    checks = 0;
    GridSpec grid{"utm19-demo-v1", "EPSG:32619", {503500, 4856500}, 1000};
    auto root = fs::path(VEYLAXIS_SOURCE_DIR);
    auto temporary = fs::temp_directory_path() /
                     ("veylaxis-catalog-" +
                      contentHash(std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))
                          .substr(0, 12));
    fs::create_directories(temporary);
    CatalogConfig config;
    config.root = (root / "S57_DATA/ENC_ROOT").string();
    config.indexFile = (temporary / "index.json").string();
    config.maxResidentCells = 2;
    config.maxCacheEntries = 2;
    auto catalog = std::make_shared<EncCatalog>(grid, config);
    check(catalog->stats().files >= 48, "Catalog discovers all ENC cells");
    check(catalog->stats().indexed == catalog->stats().files, "Cold index reads source coverage");
    auto restarted = std::make_shared<EncCatalog>(grid, config);
    check(restarted->stats().reused == catalog->stats().files && restarted->stats().indexed == 0,
          "Index restart reuses unchanged entries");
    XY position{499241.046445, 4861249.777884};
    auto owner = catalog->owner(position);
    check(owner.has_value(), "Actual land region covered");
    auto tile = value(locate(grid, position));
    auto box = value(boundsOf(grid, tile));
    auto sources = catalog->query(box);
    check(!sources.empty(), "Tile coverage query");
    double area = 0;
    for (auto source : sources)
    {
        auto region = catalog->ownedRegion(source, box);
        if (region)
            area += polygonArea(*region);
    }
    check(area <= 1000000.001 && area > 0, "Priority regions have no positive-area overlaps");
    ServiceConfig service;
    service.cacheDirectory = (temporary / "tiles").string();
    CatalogProvider provider(catalog, service);
    auto result = value(provider.request({"catalog", tile, {"land", "coastline"}}));
    check(!validate(result) && !result.features.empty(), "Catalog request returns actual data");
    auto before = provider.stats();
    auto repeated = value(provider.request({"again", tile, {"land", "coastline"}}));
    check(provider.stats().cacheHits == before.cacheHits + 1 &&
              provider.stats().cellLoads == before.cellLoads,
          "Hot request does not parse/reclip sources");
    check(repeated.metadata.requestId == "again", "Current correlation on hit");
    auto outside = value(provider.request({"outside", {grid.id, 20000, 20000}, {"land"}}));
    check(outside.metadata.layers.at("land") == LayerState::NoCoverage,
          "Outside ENC is NoCoverage, not Empty");
    // Request two more tile keys to exercise deterministic disk eviction.
    value(provider.request({"one", {grid.id, 20001, 20000}, {"land"}}));
    check(provider.stats().diskEvictions >= 1, "Disk cache obeys entry limit");
    check(provider.stats().residentCells <= 2, "Resident source count is bounded");
    fs::create_directories(root / "output/vgds");
    std::ofstream out(root / "output/vgds/catalog-validation.json");
    nlohmann::json j = {{"files", catalog->stats().files},
                        {"reused", restarted->stats().reused},
                        {"selected", catalog->entries().at(*owner).cell},
                        {"scale", catalog->entries().at(*owner).compilationScale},
                        {"features", result.features.size()},
                        {"owned_area_m2", area},
                        {"cache_hits", provider.stats().cacheHits},
                        {"disk_evictions", provider.stats().diskEvictions}};
    out << j.dump(2);
    std::cout << "VGDS S4.5: " << checks << " checks passed; cells=" << catalog->stats().files
              << " selected=" << catalog->entries().at(*owner).cell << "\n";
    return 0;
}
catch (const std::exception& e)
{
    std::cerr << "Catalog test failed: " << e.what() << '\n';
    return 1;
}
