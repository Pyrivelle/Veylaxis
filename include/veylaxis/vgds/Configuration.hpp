// 【阅读导航】启动装配结果：连接数据源、算法、网格和显示请求参数；模型映射 JSON 属于渲染层。
#pragma once
#include "S57.hpp"
#include "Catalog.hpp"
#include "IslandTerrain.hpp"

namespace veylaxis::vgds
{
struct ServiceSetup
{
    Dataset dataset;
    ServiceConfig service;
    XY start;
    std::optional<S57Report> source;
    std::optional<LandTestArea> testArea;
    std::shared_ptr<EncCatalog> catalog;
    std::shared_ptr<IslandTerrain> islands;
    std::vector<std::string> layers{"land"};
    double terrainSpacingMeters{10}, motionSpeedMetersPerSecond{150};
};

// Relative paths in configuration are resolved against the project working
// directory, consistently with the existing scripts and CLI.
Result<ServiceSetup> readServiceConfiguration(const std::string& path);
std::shared_ptr<ITileProvider> makeProvider(const ServiceSetup&, ServiceConfig);
}
