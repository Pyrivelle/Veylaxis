// 【阅读导航】数据提供者接口：客户端只认识 request()，具体海图解析、选源和缓存隐藏在实现中。
#pragma once
#include "Types.hpp"

namespace veylaxis::vgds
{
class ITileProvider
{
  public:
    virtual ~ITileProvider() = default;
    virtual TileResult request(const TileRequest& request) = 0;
};

// S1 fixed synthetic fixtures: no clipping, reconstruction, caching or networking.
class MockProvider final : public ITileProvider
{
  public:
    explicit MockProvider(GridSpec grid);
    TileResult request(const TileRequest& request) override;

  private:
    GridSpec grid_;
};
}
