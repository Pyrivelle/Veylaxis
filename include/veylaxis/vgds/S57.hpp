// 【阅读导航】单幅 S-57 读取接口：通过选项开启海底和海事对象提取，并输出来源与统计报告。
#pragma once
#include "Service.hpp"

namespace veylaxis::vgds
{
struct S57Report
{
    std::string cell, edition, update, sourceCrs, targetCrs, version;
    std::vector<std::string> sourceFiles;
    std::uint64_t landPolygons{}, landPoints{}, landLines{}, coastLines{}, unsupportedGeometries{};
    std::uint64_t geographicEdges{}, unknownEdges{};
    std::uint64_t depthAreas{}, depthContours{}, soundings{}, rejectedDepthRecords{};
    int soundingDatum{};
    double compilationScale{};
    std::map<std::string, std::uint64_t> objectCounts;
    double loadMilliseconds{};
};

struct S57Options
{
    bool bathymetry{false}, maritime{false};
};

Result<Dataset> readS57(const std::string& baseFile, const GridSpec&, S57Report* report = nullptr,
                        S57Options = {});

struct LandTestArea
{
    XY center;
    std::string sourceObjectId;
    Bounds sourceBounds;
    TileID centerTile;
};

Result<LandTestArea> selectLandTestArea(const Dataset&);
}
