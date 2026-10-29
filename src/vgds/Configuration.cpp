// 【阅读导航】配置装配层：解析网格、数据源和算法选项；在此选择单海图服务或 ENC Catalog 服务。
#include "veylaxis/vgds/Configuration.hpp"
#include "veylaxis/vgds/Grid.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <stdexcept>

namespace veylaxis::vgds
{
Result<ServiceSetup> readServiceConfiguration(const std::string& path)
try
{
    using J = nlohmann::json;
    J config;
    std::ifstream input(path);
    input >> config;
    const auto& g = config.at("grid");
    GridSpec grid{g.at("id"), g.at("crs"), {g.at("origin").at(0), g.at("origin").at(1)}, g.at("size")};
    ServiceSetup setup;
    setup.service.cacheDirectory = config.at("cache");
    setup.service.osgbEnabled = config.value("osgb", false);
    auto kind = config.contains("source") ? config.at("source").at("kind").get<std::string>() : "mock";
    if (kind == "mock")
        setup.dataset = islandFixture(grid);
    else if (kind == "s57")
    {
        S57Report report;
        auto dataset =
            readS57(config["source"].at("path"), grid, &report,
                    {config["source"].value("bathymetry", false), config["source"].value("maritime", false)});
        if (auto e = std::get_if<TileError>(&dataset))
            return *e;
        setup.dataset = std::get<Dataset>(std::move(dataset));
        setup.source = std::move(report);
    }
    else if (kind == "catalog")
    {
        const auto& source = config.at("source");
        CatalogConfig catalog;
        catalog.root = source.at("root");
        catalog.indexFile = source.at("index");
        catalog.maxResidentCells = source.value("max_resident_cells", 3u);
        catalog.maxCacheEntries = source.value("max_cache_entries", 128u);
        catalog.maxCacheBytes = source.value("max_cache_bytes", 512ull * 1024 * 1024);
        catalog.bathymetry = source.value("bathymetry", true);
        catalog.maritime = source.value("maritime", true);
        catalog.requestTimeoutMilliseconds = source.value("request_timeout_ms", 30000ull);
        setup.catalog = std::make_shared<EncCatalog>(grid, catalog);
        setup.dataset.grid = grid;
    }
    else
        throw std::invalid_argument("Unknown source kind");
    if (config.contains("layers"))
        setup.layers = config.at("layers").get<std::vector<std::string>>();
    setup.terrainSpacingMeters = config.value("terrain_spacing_m", 10.0);
    setup.motionSpeedMetersPerSecond = config.value("motion_speed_mps", 150.0);
    if (config.contains("island_terrain") && config["island_terrain"].value("enabled", false))
    {
        const auto& p = config["island_terrain"];
        IslandTerrainConfig terrain;
        terrain.enabled = true;
        terrain.heightMeters = p.value("height_m", 42.0);
        terrain.shoreScaleMeters = p.value("shore_scale_m", 200.0);
        terrain.slopeExponent = p.value("slope_exponent", 1.25);
        terrain.sampleSpacingMeters = p.value("spacing_m", 25.0);
        terrain.normalStepMeters = p.value("normal_step_m", .5);
        terrain.baseDepthMeters = p.value("base_depth_m", 12.0);
        setup.islands = std::make_shared<IslandTerrain>(terrain);
    }
    setup.start = {grid.origin.x + grid.tileSizeMeters * .5, grid.origin.y + grid.tileSizeMeters * .5};
    if (config.contains("start"))
        setup.start = {config["start"].at(0), config["start"].at(1)};
    else if (setup.source)
    {
        auto test = selectLandTestArea(setup.dataset);
        if (auto e = std::get_if<TileError>(&test))
            return *e;
        setup.testArea = std::get<LandTestArea>(test);
        setup.start = setup.testArea->center;
    }
    if (auto layers = normalizeLayers(setup.layers); std::holds_alternative<TileError>(layers))
        return std::get<TileError>(layers);
    if (!std::isfinite(setup.motionSpeedMetersPerSecond) || setup.motionSpeedMetersPerSecond <= 0)
        throw std::invalid_argument("Motion speed must be positive and finite");
    if (setup.islands)
    {
        auto count = nodeCount(grid, setup.islands->config().sampleSpacingMeters);
        if (auto error = std::get_if<TileError>(&count))
            return *error;
    }
    return setup;
}
catch (const std::exception& e)
{
    return TileError{ErrorCode::InvalidRequest, e.what()};
}

std::shared_ptr<ITileProvider> makeProvider(const ServiceSetup& setup, ServiceConfig options)
{
    if (setup.islands)
        options.sourceLoaded = [islands = setup.islands](const Dataset& dataset)
        {
            islands->addDataset(dataset);
        };
    if (setup.catalog)
        return std::make_shared<CatalogProvider>(setup.catalog, std::move(options));
    return std::make_shared<LocalService>(setup.dataset, std::move(options));
}
}
