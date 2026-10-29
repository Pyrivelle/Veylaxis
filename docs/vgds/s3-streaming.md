# S3：TileClient 与 3×3 动态场景

日期：2026-10-29。S2 验收通过后实施，S3 验收通过后按授权继续 S4。

## 文件与链路

新增公共 STL `vgds/TileClient.hpp` 和实现 TileClient.cpp；新增渲染端 `streaming/TileManager.hpp`、TileManager.cpp、TileLoader.cpp、Viewer.cpp；新增 StreamingTests.cpp 和 scripts/view-vgds.ps1。

OsgBuilder 的内存建模提取为私有 OsgMesh.hpp 接口，使 `osgb:false` 时仍可从原始语义几何在内存生成显示节点，不写 OSGB。公共 VGDS Core 不包含此私有头或任何 OSG 类型。TileManager 的 OSG 接口位于独立 streaming 目录。

修改主程序分发入口和 CMake，新增 `Veylaxis vgds --config ...`。默认无参数主程序行为保持原样。原 viewer.cpp、天空/海洋源码未修改，新入口调用既有 showTerrain 和既有环境系统。

```text
LocalService.requestJson ← TileClient 编码请求/解码校验
                                ↓ 后台单 worker
                      读 OSGB / 内存建模
                                ↓ 有界 completion 队列
                主线程 TileManager.tick 挂载/移除
```

目前使用嵌入式本地服务，共用与独立 VgdsService JSONL 相同的请求/响应编码。尚未实现跨进程 Pipe/HTTP TileClient；替换 LocalEndpoint 不改变请求和场景生命周期。

## 生命周期与资源

中心 TileID 周围固定 3×3，位置来自模拟航行输入；`--tour` 每 3 s 切换一次中心，连续穿越两个边界后返回。宿主以后可将真实船位转换后调用 tick。

状态 Unloaded→Loading→Ready→Active；离开范围直接移除并释放，不在内存保留历史缓存。失败进入 Failed，默认最多 3 次尝试、间隔 100 ms，最终失败保持可观测状态；离开再进入可重新尝试。磁盘缓存由服务处理。

队列最多 9 个任务、9 个完成结果、1 个正在执行请求。移出范围清除未开始任务；ticket 识别过时结果，正在执行的旧任务可以完成，但结果会被丢弃。完成结果先核对请求关联、TileID 和完整 GridSpec，TileClient 也检查返回图层。

唯一 worker 处理序列化服务和 OSG 文件读取，不访问活动场景。仅调用 tick/clear 的主线程挂载、移除；使用既有 Viewer 的单线程更新遍历。OSGB 关闭时内存建模也在 worker 中进行。OSG 读文件显式 CACHE_NONE，避免全局 ObjectCache 持有历史节点。

CLI 定期输出 active/loading/failed、请求次数、缓存命中、卸载/丢弃次数、上次加载耗时、活动和待提交 mesh CPU 字节数。字节数只统计 mesh 数组/索引，**不是总进程内存或显存**。源数据快照与服务缓存 IO 也占内存。GPU 对象最终释放由 OSG 图形上下文管理，自动化弱引用检查证明 CPU 场景节点已释放，不声称测量了显存。

## 构建和实际验收

```powershell
./scripts/build.ps1
. ./scripts/runtime.ps1
./build/Release/Test.exe --vgds-streaming
./scripts/test.ps1
./scripts/view-vgds.ps1 -Tour -LandOnly -SmokeTest output/vgds-s3.png

# 交互观看，默认沿用天空/海洋
./scripts/view-vgds.ps1 -Tour
```

- 本次直接测试 323 项检查通过（含异步等待轮询，计数随调度变化）。固定位置连续 tick 不重复请求；跨一块只增删三块，跨第二块共增删六块。
- 连续跨 24 个边界，活动数 ≤9，dummy mesh 活动字节稳定；原场景节点 weak observer 全部失效。
- 加载时快速切换十次中心，未开始队列保持 ≤9；旧 in-flight 完成不会挂入场景。
- 注入请求异常、畸形 JSON、关联不匹配；一次失败可重试恢复，永久失败每块只尝试配置次数，不崩溃、不无限重试。
- 实际 LocalService→JSON→OSGB→TileManager 流程通过；往返路线命中缓存、返回相同 mesh 字节，真实 OSGB 节点卸载后未被全局缓存持有。
- `osgb:false` 的纯语义→内存显示链路通过。
- 常规主程序构建成功，CTest 8/8 通过，包含 S1/S2 和原五项回归。
- 动态窗口截图 output/vgds-s3.png 已检查；日志观察到活动 9→6+3 loading→9、卸载及请求增加，XYZ 保持 1:1。图中可因中心移动显示已加载岛屿的局部，这是区块范围变化的正常结果。

## 限制

没有生产级请求超时、抢占取消和并行请求；析构等待正在执行的本地工作结束。当前单请求底层调用无独立超时。失败原因由底层 Result 产生，管理器目前公开失败计数；逐块错误详情 UI 可后续增加。OSGB=OFF 的服务构建不含渲染端；需要 Viewer 时使用正常构建并将运行配置 osgb:false，即保留内存建模但关闭文件输出。自动化不测总 RSS/显存、长时间航行帧率。未实现 S5 海底重建或 S6 点对象模型管理。
