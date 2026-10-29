// 【阅读导航】客户端请求入口：LocalEndpoint 可替换通信实现；当前 Viewer 使用进程内 JSON 调用。
#pragma once
#include "Types.hpp"
#include <functional>

namespace veylaxis::vgds
{
// Transport boundary: an embedded service today, IPC/HTTP can replace it later.
using LocalEndpoint = std::function<Result<std::string>(const std::string&)>;

class TileClient
{
  public:
    explicit TileClient(LocalEndpoint endpoint);
    TileResult request(const TileRequest&) const;

  private:
    LocalEndpoint endpoint_;
};
}
