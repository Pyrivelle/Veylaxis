# S2：裁剪、可选 OSGB 与本地缓存服务

日期：2026-10-29。阶段验收通过后继续 S3，范围遵循本轮用户授权。

## 文件与接口

- `Service.hpp`：Dataset、ServiceConfig、LocalService、clipFeature、CacheStats；公共接口只有 STL。
- `Geometry.cpp`：GDAL/GEOS 矩形求交、规范化、孔洞/MultiPolygon/线段输出。
- `IslandFixture.cpp`：迁移已有 80 点轮廓及 20/16 点孔洞，统一放大 3 倍，跨越两列区块。
- `Hash.cpp`：Windows BCrypt SHA-256；系统库，无安装新依赖。
- `LocalService.cpp`：本地同步 Provider、JSON 请求入口、磁盘缓存和完整性检查。
- `OsgBuilder.hpp/.cpp`：独立 VgdsOsgBuilder 静态构建目标；Earcut 仅在此使用。
- `ServiceMain.cpp`、`config/vgds-mock.json`：VgdsService JSONL 标准输入/输出入口。
- 修改 Types/JsonCodec：TilePackage 新增可选 artifacts；旧 schema 1 数据仍可读取。
- `ServiceTests.cpp`、服务专用测试入口、CMake 配置；原核心构建分支不增加 GIS/OSG 依赖。
- `third_party/`：复制已有 Earcut 原文件、ISC/MIT 许可，原 perevia-osgkit 未修改。

服务本地入口采用函数调用或 JSONL CLI，没有 HTTP、监听端口和下载网络框架。CLI 每行一个 S1 TileRequest，每行返回 TilePackage，非法请求返回 error 对象；配置和读取 ENC 失败时进程返回非零。当前错误封装是 CLI 适配层约定，不改变核心 Result 类型。

```powershell
. ./scripts/runtime.ps1
'{"schema":1,"requestId":"demo","tile":{"gridId":"utm19-demo-v1","x":0,"y":0},"layers":["land"],"terrainSpacingMeters":10}' | ./build/Release/VgdsService.exe --config config/vgds-mock.json
```

## 数据与边界

sourceObjectId 在跨块片段之间相同；fragmentId 包含来源身份、网格和 TileID。规范化几何输出，不依靠 GEOS 返回顺序给一个 MultiPolygon 的各部分另造来源身份。

原始边段上的输出边继承来源标记，新矩形边标 ArtificialClip；判定距离容差 1e-7 m，矩形交点在 1e-8 m 内对齐。孔洞切开时允许形成外环凹口；面接触产生的零面积线/点不会成为陆地面。点仍按半开区间唯一归属。二维面求交不实现海底重建。

OSGB 为可丢弃缓存，包内始终保留语义几何。模型内部 XY 是相对 tileOrigin 的米制坐标，文件元数据含原点和 Z×1。artifacts 只含文件路径、格式、Builder 版本、SHA-256、字节数、原点与 Z 比例；没有 OSG 类型进入 Core。

显示采用统一 12 m 平顶，底部 0 m，明确标为 Estimated display，不是 S57 实测地形。顶面法线统一向上；仅 Geographic 边生成侧壁，人工切边无侧壁、无向海岸降高。平顶避免独立三角剖分带来的接缝高度差。未来真实岛屿地形应基于原对象的统一高度场；本轮没有声称提供真实陆地高程。

## 缓存

键覆盖来源几何/属性/数据版本、完整 GridSpec、规范化图层、TileID、采样参数、算法版本、OSGB 开关与 Builder 版本，不包含请求关联 ID。缓存命中时恢复当前 requestId。

缓存目录按 SHA-256 分组，tile.json 为原始语义包，可选 tile.osgb，commit.json 保存包的完整性摘要。数据与 manifest 临时文件先写后提交，manifest 最后提交；读取校验包摘要、结构和 OSGB 摘要/大小。损坏项自动重建，不把错误当空块缓存。线程内互斥、跨进程文件锁保护写入；其他进程同时使用同一缓存时返回错误而非等待。当前服务适合单个本地实例；磁盘缓存无容量淘汰策略，属于后续优化。

配置 `osgb:false` 关闭生成。构建 `-DVEYLAXIS_VGDS_OSGB=OFF` 完全不创建 Builder。`-DVEYLAXIS_VGDS_SERVICE_ONLY=ON` 跳过 Qt 和天空/海洋 SDK；配合 OSGB=OFF 不查找 OSG。S1 的 VGDS_ONLY 优先早期返回，仅构建原纯核心测试。

## 实际验收

```powershell
./scripts/build.ps1
. ./scripts/runtime.ps1
./build/Release/Test.exe --vgds-service
./scripts/test-vgds.ps1
./scripts/test.ps1
```

- S2：264 项检查通过。约 1,014,350 m² 的带孔岛屿跨六个测试块面积守恒（误差阈值 1e-5 m²）；公共边界区间一致。
- 包含保留孔洞、跨缝孔洞、单一凹多边形裁成两个连通分量、不同片段身份、原始属性/网格变化失效、重复请求、进程重启、损坏 JSON/OSGB 修复。
- OSGB 反读顶面双精度局部坐标、公共边界 XYZ 一致、切边无侧壁、Z×1，artifact JSON 往返。
- 纯核心 CTest 1/1 通过；正常主程序构建成功，CTest 7/7 通过，包含原五项功能测试。
- build/vgds-no-osg：SERVICE_ONLY=ON、OSGB=OFF 构建和 CTest 2/2 通过。
- CLI 实测输出 output/vgds-s2-cli.jsonl：两次相同请求一生成一命中；非法第三行返回结构化错误。

## 限制

无 HTTP/远程下载、生产级服务限流和磁盘淘汰；使用 Windows 系统哈希/文件提交 API。原始源对象 bbox 先筛选再求交，尚无树形空间索引。S2 模拟来源版本明确标 Synthetic。公用边界及面拓扑依赖现有 GDAL/GEOS；不沿用只适合单次竖切的示例算法。
