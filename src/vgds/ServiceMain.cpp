// 【阅读导航】可独立运行的本地服务入口：每行读取一个 JSON 请求并输出响应；当前不是 HTTP 服务。
#include "veylaxis/vgds/Service.hpp"
#include "veylaxis/vgds/Codec.hpp"
#include "veylaxis/vgds/Configuration.hpp"
#ifdef VEYLAXIS_HAS_OSGB
#include "veylaxis/vgds/OsgBuilder.hpp"
#endif
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <stdexcept>

int main(int argc, char** argv)
try
{
    using namespace veylaxis::vgds;
    using J = nlohmann::json;
    if (argc != 3 || std::string(argv[1]) != "--config")
    {
        std::cerr << "VgdsService --config config.json < requests.jsonl > responses.jsonl\n";
        return 1;
    }
    auto definition = readServiceConfiguration(argv[2]);
    if (auto e = std::get_if<TileError>(&definition))
        throw std::runtime_error(e->message);
    auto setup = std::get<ServiceSetup>(std::move(definition));
    auto options = setup.service;
#ifdef VEYLAXIS_HAS_OSGB
    options.builder = buildOsgb;
    options.builderVersion = OsgBuilderVersion;
    if (setup.islands)
    {
        options.builder = proceduralBuilder(setup.islands);
        options.builderVersion = setup.islands->version();
    }
#endif
    if (setup.source)
        std::cerr << "S57 cell=" << setup.source->cell << " edition=" << setup.source->edition
                  << " update=" << setup.source->update << " land_polygons=" << setup.source->landPolygons
                  << " land_points=" << setup.source->landPoints << " land_lines=" << setup.source->landLines
                  << " coastline=" << setup.source->coastLines
                  << " unsupported=" << setup.source->unsupportedGeometries
                  << " load_ms=" << setup.source->loadMilliseconds << '\n';
    if (setup.testArea)
        std::cerr << "Land test source=" << setup.testArea->sourceObjectId
                  << " tile=" << setup.testArea->centerTile.x << "," << setup.testArea->centerTile.y << '\n';
    auto provider = makeProvider(setup, options);
    std::string line;
    while (std::getline(std::cin, line))
    {
        if (line.size() > 1024 * 1024)
        {
            std::cout << J{{"error",
                            {{"code", static_cast<int>(ErrorCode::InvalidRequest)},
                             {"message", "Request too large"}}}}
                             .dump()
                      << '\n';
            continue;
        }
        auto result = requestProviderJson(*provider, line);
        if (auto e = std::get_if<TileError>(&result))
            std::cout << J{{"error", {{"code", static_cast<int>(e->code)}, {"message", e->message}}}}.dump()
                      << '\n';
        else
            std::cout << std::get<std::string>(result) << '\n';
        std::cout.flush();
    }
    if (auto service = std::dynamic_pointer_cast<LocalService>(provider))
    {
        auto s = service->stats();
        std::cerr << "requests=" << s.requests << " hits=" << s.hits << " generations=" << s.generations
                  << " repairs=" << s.repairs << '\n';
    }
    if (auto catalog = std::dynamic_pointer_cast<CatalogProvider>(provider))
    {
        auto s = catalog->stats();
        std::cerr << "cells=" << s.files << " source_loads=" << s.cellLoads << " resident=" << s.residentCells
                  << " cache_hits=" << s.cacheHits << " generations=" << s.generations << '\n';
    }
    return 0;
}
catch (const std::exception& e)
{
    std::cerr << e.what() << '\n';
    return 1;
}
