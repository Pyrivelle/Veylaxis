// 【阅读导航】协议接口：请求与区块包 JSON 编解码、数据合法性检查，不涉及模型加载。
#pragma once
#include "Types.hpp"

namespace veylaxis::vgds
{
Result<std::string> encodeJson(const TilePackage& package);
TileResult decodeJson(const std::string& encoded);
Result<std::string> encodeRequestJson(const TileRequest& request);
Result<TileRequest> decodeRequestJson(const std::string& encoded);
}
