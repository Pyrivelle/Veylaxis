#pragma once
#include "veylaxis/vgds/Service.hpp"

namespace veylaxis::vgds
{
// Called under its provider's mutex. The cache is process-local; paths are local.
class TileCache
{
  public:
    TileCache(std::string root, std::size_t entries, std::uint64_t bytes);
    TileResult fetch(const std::string& key, const TileRequest&,
                     const std::function<TileResult(const std::string& osgbPath)>& generate);
    std::uint64_t hits{}, generations{}, evictions{}, repairs{};

  private:
    std::string root_;
    std::size_t entries_;
    std::uint64_t bytes_;
    void prune(const std::string& keep);
};
}
