# VGDS S0：工程检查与接口设计准备

日期：2026-10-29。范围：只执行 S0，未实现 S1–S7，未修改业务代码、构建配置或 perevia-osgkit，未安装依赖。此次新增文件仅为本检查报告。测试运行更新了既有 build 测试成果。

已阅读 `C:/Users/jitia/Downloads/Veylaxis_VGDS_Codex_Implementation_Plan.md`。该文档作为待确认的设计输入；本次执行范围以用户明确指定的 S0 为准，后续阶段的任务描述不构成本次实施授权。

## 1. 已验证现状

| 项目 | 实际状态 |
| --- | --- |
| Veylaxis 结构 | C++17，根目录 CMakeLists.txt；Veylaxis 主程序、Test 测试程序。没有独立 VGDS 目标或公共数据协议 |
| 业务入口 | src/main.cpp：view/reconstruct；src/bathymetry.cpp：S57 读取、投影、裁剪、重建、文件输出；src/viewer.cpp：TIFF/OSGB 读取与 OSG 建模、显示 |
| 场景根节点 | showTerrain 创建 osg::Group，再挂海底、SilverLining 天空和 Triton 海洋；它仍在 Viewer 函数局部管理 |
| 多块成果 | tests/tile_scene.cpp 和 Python 调度实现静态九块检查、模型组装；不是运行时 TileManager、动态请求或缓存服务 |
| 通信/异步 | 在所检查的 src/include/tests 中未发现 TileClient、TileManager、HTTP 服务或业务工作队列。当前 Viewer 使用单线程 OSG；不能将脚本批处理视为异步服务 |
| 算法接口 | include/veylaxis/reconstruction.hpp 的 XY、DepthBand、ReconstructionInput、ISeabedReconstructor 等没有 OSG/GDAL/Qt 依赖 |
| 地理处理 | bathymetry.cpp 已使用 OGR Intersection、空间过滤、EPSG:4326→32619、GDAL 栅格化、halo 和 NoData；这些逻辑尚封装在 CLI 中 |
| 身份信息 | 源提取成果主要保存 source_fid，未形成 cell+LNAM 的稳定 Feature 身份和片段身份协议 |
| 裁剪工程 | C:/CProj/perevia-osgkit 可访问，为另一独立 Git 工程；未向其写入或改动 |
| 模拟数据 | tests/IslandExample.h 存在确定性的 80 点外环及两个 20/16 点孔洞。类型为 IslandOptions/osg::Vec2d，并附带演示高度参数；不是纯二维地理数据文件 |
| Earcut | 仅 perevia-osgkit 有 vendored mapbox/earcut.hpp，Veylaxis 目前未直接接入 |

### 构建与依赖

- Veylaxis 缓存指向 Visual Studio 18 2026、MSVC 19.52.37025.0、C++17、x64-windows。
- 当前 vcpkg installed 根目录为 `C:/CProj/vcpkg/installed`。OSG 3.6.5、GDAL 3.12.4、GEOS 3.14.1；两项目的主要 OSG 配置均使用该配套。
- Veylaxis 使用 `find_package(unofficial-osg CONFIG)`；perevia 使用 `find_package(OpenSceneGraph 3.6.5)` 和其 INTERFACE 依赖适配目标。应按实际 include/lib/DLL 路径核对一致性，不能仅因查找方式不同就判为 ABI 不兼容。
- Veylaxis 顶层无 vcpkg.json；perevia 有只声明 osg 的 manifest，Earcut 为私有头文件依赖。
- 已有 nlohmann_json、json-c、Asio 安装目录；未找到顶层 include/httplib.h 或 httplib 安装目录。存在 Asio 不代表已经有 HTTP 服务实现。
- 现有 Veylaxis 顶层无条件查找 OSG、GDAL、Qt、SilverLining/Triton。即使新增纯数据源文件，也不能据此宣称可脱离渲染 SDK 单独配置构建。
- 天空/海洋使用既有本机 SDK 和 Qt，Release 验证可用；不要在 S1 顺带改其部署、授权或图形配置。
- Earcut 固定提交：177bd66b5338ab66f4e9ce8f3cca418b9463d809；实算 SHA-256 与本地 README 一致：29091c0d7af22a2f985f5ebd9aedb1f1b056fe099d1d31395c393c5f1ecb8c52。许可 ISC；perevia 工程许可 MIT，复用时保留适当许可声明。

### 输入与实际海图

`US2EC04M.000`（2,130,523 字节）和 `.001`（414 字节）均可访问。通过本机 QGIS 自带 GDAL 以只读模式、UPDATES=APPLY 实际检查：驱动 S57，62 个图层。

- LNDARE、COALNE、SOUNDG、DEPCNT、DEPARE、FAIRWY 存在。
- DRGARE 图层在此单元中不存在，不能把规划中的对象清单当作该单元的实际图层清单。
- 原约 1 km² 水深实验范围中 LNDARE 数为 0；S4 必须另选实际包含陆地的范围。
- 本次查询工具使用 QGIS 配套 GDAL，Veylaxis 构建仍使用 vcpkg GDAL；没有混用进程内 DLL。

## 2. 裁剪代码可复用到什么程度

`perevia-osgkit/tests/IslandSplitTests.cpp` 的 clip() 是单竖直半平面的 Sutherland–Hodgman 示例，且显式断言海岸只与切线相交两次。它输出单个 Ring，位于测试文件匿名命名空间，不是公开的矩形/MultiPolygon 裁剪 API。

本次实际执行 Release 的 `--island-split-test`：80 个原始点，cutX≈-1.63943，左右 41/43 点；细分后 9984/10496 面；17 对接边顶点位置和法线误差均为 0，面积和约 119056。它证明该固定外环实例可行；没有包含原示例的两个孔洞，也没有证明任意凹多边形的多个连通分量能正确返回。

建议按以下边界复用：

| 代码 | 建议用途 | 不应直接做的事 |
| --- | --- | --- |
| IslandExample.h | S2 导出/转换成 double 二维模拟 fixture，保留两个孔洞和来源标记 | 将 OSG 类型或演示 42 m 高度带进 VGDS 公共模型并当实测 |
| IslandSplitTests.cpp | 面积守恒、公共交点一致、世界坐标和法线检查用例；沿原始边方向计算交点的思路 | 直接承担带孔、多连通任意矩形求交 |
| IslandBuilder.cpp | 主程序侧 Earcut 适配、环验证、索引/朝向/面积检查逻辑 | 将返回 osg::Node 的 IslandBuilder 当作 VGDS Provider |
| TerrainBuilder | 主程序侧未来可评估复用 | 直接处理当前 NoData 和缓存边界法线：现接口拒绝 NoData，行沿 +Y，且局部色带/法线/LOD 仍需适配 |
| Veylaxis bathymetry.cpp | S4/S5 提取 S57/OGR 几何查询、投影、Intersection 和高程流程 | S1 大规模迁移整个函数或让服务启动依赖天空/海洋 |
| reconstruction.hpp/.cpp | S5 保留既有无 OSG 算法接口 | 现在把水深算法塞进 S1 数据模型阶段 |
| tile_scene.cpp、tile-experiment.py | 后续规则网格、共同原点、halo、法线接缝的回归样例 | 当作已实现的服务端缓存、请求调度或客户端动态加载 |

Earcut 的作用是三角剖分，不是多边形布尔裁剪。S2 优先封装现有 GDAL/GEOS 求交能力，保留 Polygon 的外环、孔洞和 MultiPolygon；Earcut 先留在主程序渲染适配侧。

另一个关键经验：裁切线不能成为新的“海岸”。原 IslandBuilder 按到输入岸线的距离造山，直接对裁出的每片调用它会压低切边并生成内部侧壁。模拟高度/法线需基于原始来源轮廓或统一场，而非每块新轮廓。

## 3. 建议目录（未创建）

```text
include/veylaxis/vgds/
  Types.hpp                 # S1：请求、元数据、几何和数据包
  Grid.hpp                  # S1：网格定义与定位
  ITileProvider.hpp         # S1：同步核心接口
  Codec.hpp                 # S1：序列化边界
src/vgds/
  Grid.cpp                  # S1
  MockProvider.cpp/.hpp      # S1：固定模拟响应，非岛屿裁剪服务
  JsonCodec.cpp             # S1
  geometry/                 # S2：矩形求交适配
  cache/                    # S2/S7
  transport/                # S2：薄 HTTP 入口
  sources/                  # S4：S57；S5：terrain
src/tiles/                  # S3：TileClient、TileManager、OsgTileBuilder
tests/vgds/                 # S1 核心契约测试；后续裁剪/缓存测试
tests/data/vgds/             # S2：纯数据模拟岛屿 fixture
docs/vgds/                  # 协议、坐标和阶段记录
```

目录是源码职责划分，不为每个类生成库或 VS 项目。S1 可将同一组 core 源文件编译进现有 Veylaxis/Test；另提供 VGDS-only 配置路径，仅构建纯数据 Test。S2 真正需要独立进程时再增加一个薄 VgdsService 可执行目标。

## 4. 接口草案（待确认，不是已实现 API）

| 类型/接口 | 必要语义 |
| --- | --- |
| GridSpec | gridId、CRS、固定 double 米制原点、tileSize=1000、版本；当前仅 UTM19N，非全球网格 |
| TileID | gridId + int64 x/y；不是文件路径或几何中心哈希 |
| TileRequest | TileID、规范化图层集合、terrainSpacingMeters、请求/处理配置版本；请求 ID 供后续过时响应判断 |
| TileMetadata | schema/source/algorithm 版本、全局 bounds、tileOrigin、水平 CRS、层状态、来源类型及覆盖信息 |
| TerrainData | columns/rows、正的 spacing、样本起点、明确行列方向、Float32 Z、有效性掩码、垂直基准、Estimated/Synthetic 标识；可选 halo-derived normals |
| Feature/Geometry | double XY，Point/LineString/Polygon/MultiPolygon；Polygon 明确外环和 holes；原始属性与边界来源保留 |
| Feature identity | sourceId=数据集+ENC 单元+LNAM（有定义的回退策略）；fragmentId 再结合 TileID、几何部分；来源版本单独保存 |
| TilePackage | metadata + 按需 terrain + features，不含 osg::Node、OGRGeometry、QString 或 GDALDataset |
| TileError | UnsupportedLayer、InvalidGrid/Resolution、NoCoverage、SourceUnavailable、GenerationFailed 等；不使用空数组代替所有错误 |
| LayerStatus | Available、AvailableEmpty、Unavailable/NoCoverage 等逐层状态；支持部分成功，不把缺失图层说成请求整体失败 |
| ITileProvider | `TileResult request(const TileRequest&)`；C++17 可用 variant/显式结果包装，不使用 C++23 expected |
| Codec | 数据包↔JSON/数组编码；内部 JSON 库不暴露为核心公共数据类型 |

S1 的 request 先同步便于测试；异步调度和取消放在后续客户端/作业层。请求状态 PENDING 与完成后的 TilePackage 分开，不把线程/OSG 生命周期塞进 Provider。

建议默认网格沿用九块实验：原点 `(503500,4856500)`，EPSG:32619，Tile(0,0) 为旧区域中心附近的精确 1 km 方块。这是待确认方案。点对象使用半开范围 `[min,max)`，索引用 floor，负坐标不能用截断。线面允许在相邻块保留一致的公共边界。

S5 文档的 10 m→101×101 和既有 5 m→201×201 应通过 request 参数共存；要求 1000/spacing 为整数，节点数为该商+1。建议 wire 层明确行从北到南，以减少与当前 GDAL 数据转换；PereVia TerrainBuilder 行从南到北，必须在渲染适配层翻行。Z 明确向上、depth 向下为正，转换只执行一次。

外环/孔洞本身没有实测高程时不填成真实地形。NaN 不直接写进 JSON 数字；用掩码与明确的无效值编码，二进制数组时规定 Float32、小端、尺寸、长度和上限。缺测高程严禁静默填 0。

## 5. 技术问题与选择

1. 通用裁剪还未实现为公共接口；现验证仅外环双片。S2 需新增矩形四边、多个交点、孔洞穿块、多个分量、仅接触边角、退化结果等测试。
2. 模拟岛屿约数百米，原坐标在零点附近；需显式平移到模拟网格边界附近，才能测试两个 1000 m 块。不能假定已有示例天然跨当前 UTM 网格。
3. 身份不能用 FID/double 代替 LNAM。当前 source_fid 适用于诊断提取；协议需要精确 ID、稳定组件编号、更新链版本，以及人工切边与真实海岸的区别。
4. 服务依赖隔离：应调整 CMake 的顶层依赖查找门控，纯核心测试不配置 OSG/Qt/天空/海洋；公共头无 OSG 只是第一步。
5. 当前 Test 包含相机和 OSG 模型测试，因此不能直接宣称它是 VGDS 无渲染测试。VGDS-only 配置应只包含纯数据测试源，正常配置继续保留旧 Test 内容。
6. 不直接链接整个 PereViaOsg DLL 进入服务；其公共 API 依赖 OSG。复用样例数值与算法时转换成 STL 类型，并保留 MIT/ISC 许可。
7. 已有 halo/共享法线方案是当前参数下的结果，不代表任意算法/LOD 无缝。此前九块内部仍存在约 11.64 m 的相邻节点台阶，服务不能通过缓存掩盖它或将其标记成实测 DEM。
8. 点归属、几何接触容差、环规范化、MultiPolygon 顺序会影响缓存/片段 ID；采用统一规则，不依赖每次 GEOS 输出顺序或每块独立归一化。
9. 后续 GDAL/OGR 并行作业需独立 dataset/读取游标，不能复用一个会修改空间过滤器的对象。全局初始化与 SDK 生命周期不进入逐请求循环。
10. 未发现现成 HTTP 层。建议 S1 不引入网络依赖；S2 再选择小型、固定版本的 HTTP 适配（候选 cpp-httplib），仅监听 127.0.0.1，端口可配置。现有 Asio 也可作为候选基础，但不是完整 HTTP 框架。本轮不安装或启动服务、不占用端口。

## 6. 下一阶段 S1 最小实施计划

待用户确认后，第一处新增代码建议为 `include/veylaxis/vgds/Types.hpp`，先写数据契约，再写 Grid 和接口，不先迁移 bathymetry.cpp。

预计新增：Types/Grid/ITileProvider/Codec 公共头、Grid/MockProvider/JsonCodec 实现、tests/vgds 的网格/筛选/错误/序列化测试和协议说明。仅修改根 CMakeLists.txt、tests/main.cpp 及必要测试入口；为 VGDS-only 模式增加构建门控，保持正常 Veylaxis/Test 的行为。JSON 实现可复用已安装 nlohmann_json，作为编解码实现依赖，不新增下载。

S1 MockProvider 返回确定性的模拟包来验证契约和筛选，不实现 80 点岛屿裁剪、HTTP、缓存、S57 Provider、TileManager 或新的海底插值算法。岛屿数据迁移到 S2 再执行。

验收用例：

- 网格原点、1000 m 正边界、负边界与 corner 四邻点的唯一点归属。
- 相同 TileID/请求规范化稳定，重复/乱序 layers 处理明确；非法分辨率被拒绝。
- 空图层、未知图层、缺测与请求错误可区分；无效高程不会变为有效 0。
- 内外环、多几何与属性/身份的编码往返；Terrain 小端数组/掩码维度、版本和长度校验；这不是 S2 裁剪实现。
- 5 m/10 m 节点布局公式正确，共享边界坐标一致；模拟来源标记保留。
- VGDS-only 在不查找/链接 OSG、GDAL、Qt、SilverLining/Triton、不创建窗口的条件下通过；正常 Release 回归不退化。

S1 拟增加的 VGDS-only CMake 开关名称和具体文件列表应在开始该阶段时确认；当前尚不存在此开关，不能把它当成已可运行命令。

## 7. 本次实际验证及可运行指令

```powershell
# Veylaxis 现有构建入口（本轮只检查配置，未执行干净重建）
cd C:\CProj\Veylaxis
.\scripts\build.ps1 -VcpkgRoot C:\CProj\vcpkg -Configuration Release

# 本轮实际执行：既有 Release 二进制，5/5 通过，总计 29.35 s
.\scripts\test.ps1 -Configuration Release

# 本轮实际执行：不创建图形窗口，裁剪/接缝数值检查通过
$env:PATH = 'C:\CProj\vcpkg\installed\x64-windows\bin;' + $env:PATH
& 'C:\CProj\perevia-osgkit\build\Release\PereViaOsgTests.exe' --island-split-test
```

没有宣称新 VGDS 模块已构建：新接口只是本报告中的草案。未运行 HTTP、动态块加载、S4 陆地显示或 S5 新高度图生成。既有回归测试会重新生成 build/test-region001 内的旧功能测试数据。

## 8. 待确认后再推进

建议确认：①采用实验网格原点 `(503500,4856500)` 与 1000 m 网格；②S1 仅实现纯数据契约、Mock、编解码及测试；③核心使用 STL，JSON 编解码采用已有 nlohmann_json，几何求交后续优先 GDAL/GEOS，Earcut 留在主程序侧。HTTP 框架和端口可延至 S2 确认，不阻塞 S1。

S0 完成，已暂停，等待用户确认；不自动进入 S1。
