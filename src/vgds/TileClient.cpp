// 【阅读导航】客户端协议边界：把请求编码为 JSON，调用本地端点，再校验响应的请求号、Tile 和图层。
#include "veylaxis/vgds/TileClient.hpp"
#include "veylaxis/vgds/Codec.hpp"
#include "veylaxis/vgds/Grid.hpp"
#include <stdexcept>

namespace veylaxis::vgds
{
TileClient::TileClient(LocalEndpoint endpoint) : endpoint_(std::move(endpoint))
{
    if (!endpoint_)
        throw std::invalid_argument("Missing local endpoint");
}
TileResult TileClient::request(const TileRequest& r) const
try
{
    auto encoded = encodeRequestJson(r);
    if (auto e = std::get_if<TileError>(&encoded))
        return *e;
    auto response = endpoint_(std::get<std::string>(encoded));
    if (auto e = std::get_if<TileError>(&response))
        return *e;
    auto result = decodeJson(std::get<std::string>(response));
    if (auto p = std::get_if<TilePackage>(&result))
    {
        if (!(p->metadata.tile == r.tile) || p->metadata.requestId != r.requestId)
            return TileError{ErrorCode::InvalidPackage, "Response does not match request"};
        auto requested = std::get<std::vector<std::string>>(normalizeLayers(r.layers));
        std::vector<std::string> returned;
        for (const auto& l : p->metadata.layers)
            returned.push_back(l.first);
        if (requested != returned)
            return TileError{ErrorCode::InvalidPackage, "Response layers do not match request"};
    }
    return result;
}
catch (const std::exception& e)
{
    return TileError{ErrorCode::IoFailure, e.what()};
}
}
