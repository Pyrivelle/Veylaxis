// 【阅读导航】海底重建的数据适配接口：组织深度区域、等深线和测深点，与 OSG 场景分离。
#pragma once
#include "Service.hpp"
#include "veylaxis/reconstruction.hpp"

namespace veylaxis::vgds
{
struct DepthAreaInput
{
    std::string sourceId;
    Geometry footprint;
    double lower{}, upper{};
};

struct SeabedInput
{
    std::vector<DepthAreaInput> areas;
    std::vector<veylaxis::Segment> contours;
    std::vector<veylaxis::Sounding> soundings;
    std::string verticalDatum;
};

TerrainSource makeSeabedSource(SeabedInput);
}
