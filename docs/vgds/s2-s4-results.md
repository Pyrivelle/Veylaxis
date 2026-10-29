# VGDS S2、S3、S4 最终验收

2026-10-29。按本轮授权顺序实施，各阶段验收通过后进入下一阶段；当前停在 S4，未实现 S5–S7。

## 阶段结果

| 阶段 | 交付 | 独立验收 |
| --- | --- | --- |
| S2 | 80 点模拟岛屿、孔洞/MultiPolygon 矩形裁剪、逐边身份、可选独立 OSGB Builder、SHA-256 磁盘缓存、本地函数/JSONL 入口 | 264 项检查；带孔总面积守恒、公共边界 XYZ、无切边侧壁、跨缝孔洞、多连通、命中/重启/损坏修复、OSGB 开关 |
| S3 | TileClient、单 worker、TileManager、3×3 动态场景、异步加载卸载、失败重试、过时任务丢弃、内存建模回退 | 连续 24 个边界；活动≤9；重复位置不重复请求；移出节点 weak observer 失效；真实 OSGB 不被全局缓存持有；异常/终止重试/快速改向；动态窗口截图 |
| S4 | 一次加载真实 ENC、应用 .001、投影和 LNDARE/COALNE 转换、来源属性、真实跨块测试区、共用原客户端 | 全部 590 个 LNDARE 和 347 个 COALNE 来源信息；真实岛屿跨 3 块，面积误差 6.1e-8 m²；相邻边界一致；删除测试源副本后仍可生成请求；原 S3 生命周期通过；陆地和融合窗口截图 |

详细文件改动、接口、语义和限制见 [S2](s2-service.md)、[S3](s3-streaming.md)、[S4](s4-s57.md)。本次没有修改 perevia-osgkit、天空/海洋源码或原 viewer.cpp，没有安装新依赖。Earcut 复用现有固定版本，保留 ISC/MIT 许可。

## 最终构建验证

| 配置/入口 | 实际结果 | 日志 |
| --- | --- | --- |
| 正常主程序和 Test | 构建成功，CTest 9/9 通过，含原五项回归 | build/Testing/Temporary/LastTest.log |
| 纯 Core：VGDS_ONLY | 401 项检查，CTest 1/1 通过；dumpbin 仅 Windows/MSVC 运行库 | build/vgds-core/Testing/Temporary/LastTest.log |
| SERVICE_ONLY，OSGB=ON | 无 Qt/海洋 SDK，CTest 4/4 通过 | build/vgds-service/Testing/Temporary/LastTest.log |
| SERVICE_ONLY，OSGB=OFF | 无 OSG/Qt/海洋 SDK 查找，CTest 3/3 通过 | build/vgds-no-osg/Testing/Temporary/LastTest.log |
| 根 Veylaxis.slnx | MSBuild Release/x64 成功，五个必要目标可构建 | 本轮命令输出 |
| OSG 窗口 | 模拟动态陆地、真实陆地、真实陆地+SilverLining/Triton 启动/截图/退出成功 | output/vgds-s3.png、vgds-s4-land.png、vgds-s4-marine.png |

真实来源报告：output/vgds/s4-validation.json，关闭 OSGB 对照：output/vgds/s4-validation-no-osgb.json。异步测试计数包含轮询次数，会随线程调度变化；以测试通过、场景/队列上限和资源释放断言为验收依据。

## 使用

在项目根目录执行：

```powershell
./scripts/build.ps1
./scripts/test-vgds.ps1
./scripts/test-vgds-service.ps1
./scripts/test-vgds-service.ps1 -NoOsgb
./scripts/test.ps1

# 模拟航行，显示天空海洋与陆地
./scripts/view-vgds.ps1 -Config config/vgds-s57.json -Tour

# 单独观察完整初始陆地
./scripts/view-vgds.ps1 -Config config/vgds-s57.json -LandOnly
```

把配置文件里的 osgb 改为 false，可关闭 OSGB 文件输出，主程序仍从语义数据在后台建模。构建 VEYLAXIS_VGDS_OSGB=OFF 可排除 Builder；当前此纯服务构建不提供 VGDS Viewer。

真实陆地区域初始中心 TileID 为 (-5,4)，XY=(499241.046445,4861249.777884)，EPSG:32619。网格原点和尺寸始终取自配置；单个服务请求只生成一个区块，3×3 由客户端发起九次独立请求组成。

## 未解决事项

- 当前是估算 12 m 平顶陆地，Z×1；没有真实陆地高程、山形或新的海底重建，OSGB 可随时重新生成，原始几何始终保留。
- 本地函数/JSONL 服务与嵌入式 TileClient 已实现；没有 HTTP/跨进程客户端、运行时 ENC 热更新、多图幅拼接、覆盖区域 M_COVR 或磁盘缓存淘汰。
- 97 条未被 COALNE 确认的源边保留 Unknown，不生成侧壁；其他 ENC 的几何类型仍需按实际输入检查。
- 运行中任务尚无可抢占取消/底层独立超时；当前单 worker 有界，析构等待正在执行任务结束。没有总 RSS/显存和长期帧率基准，mesh 字节统计不等于进程总内存。
- 1:675000 海图的轮廓精度不因 1 km 分块而提升；后续地形阶段仍需明确数据精度和垂直基准。

本轮完成并暂停，等待下一阶段确认。
