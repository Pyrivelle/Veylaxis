// 【阅读导航】协作式取消：请求上下文保存在当前线程，耗时循环主动检查取消或超时。
#include "veylaxis/vgds/Service.hpp"

namespace veylaxis::vgds
{
namespace
{
thread_local std::function<bool()> cancelled;
}

RequestScope::RequestScope(std::function<bool()> current) : previous_(std::move(cancelled))
{
    cancelled = std::move(current);
}

RequestScope::~RequestScope()
{
    cancelled = std::move(previous_);
}

void requestCheckpoint()
{
    if (cancelled && cancelled())
        throw RequestCancelled();
}
}
