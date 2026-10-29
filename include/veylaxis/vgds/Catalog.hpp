// 【阅读导航】海图目录接口：EncCatalog 负责覆盖索引，CatalogProvider 实现面向 Tile 的数据请求。
#pragma once
#include "Service.hpp"

namespace veylaxis::vgds
{
struct CatalogEntry
{
    std::string cell, path, version, edition, update;
    double compilationScale{};
    Bounds bounds;
};

struct CatalogStats
{
    std::uint64_t files{}, indexed{}, reused{}, cellLoads{}, cellEvictions{}, residentCells{}, cacheHits{},
        generations{}, diskEvictions{};
};

struct CatalogConfig
{
    std::string root, indexFile;
    std::size_t maxResidentCells{3}, maxCacheEntries{128};
    std::uint64_t maxCacheBytes{512ull * 1024 * 1024};
    bool bathymetry{false}, maritime{false};
    std::uint64_t requestTimeoutMilliseconds{30000};
};

class EncCatalog
{
  public:
    EncCatalog(GridSpec, CatalogConfig);
    ~EncCatalog();
    const GridSpec& grid() const;
    const CatalogConfig& config() const;
    const std::vector<CatalogEntry>& entries() const;
    std::vector<std::size_t> query(const Bounds&) const; // actual coverage intersection, finest first
    std::optional<std::size_t> owner(XY) const;
    std::optional<Geometry> ownedRegion(std::size_t, const Bounds&) const;
    CatalogStats stats() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class CatalogProvider final : public ITileProvider
{
  public:
    CatalogProvider(std::shared_ptr<EncCatalog>, ServiceConfig);
    ~CatalogProvider();
    TileResult request(const TileRequest&) override;
    void cancel(const std::string& requestId);
    CatalogStats stats() const;
    const EncCatalog& catalog() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
