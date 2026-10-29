# VGDS S1：纯数据核心

阶段：S1。日期：2026-10-29。范围：数据模型、可配置网格、同步 Provider、固定 Mock、请求/响应 JSON 编解码、独立核心测试。没有实现 HTTP、S57 Provider、真实几何裁剪、地形重建、缓存或动态加载；没有修改 perevia-osgkit。

## 文件改动

新增：

- `include/veylaxis/vgds/Types.hpp`：纯 STL 数据模型、几何/边界/来源/状态/错误类型。
- `include/veylaxis/vgds/Grid.hpp`：网格验证、定位、逻辑范围、节点数、图层规范化。
- `include/veylaxis/vgds/ITileProvider.hpp`：同步 Provider 与固定 Mock 声明。
- `include/veylaxis/vgds/Codec.hpp`：请求和 TilePackage 的 JSON 接口。
- `src/vgds/Grid.cpp`、`MockProvider.cpp`、`JsonCodec.cpp`：实现。
- `tests/vgds/CoreTests.cpp`、`main.cpp`：独立契约测试与入口。
- `cmake/VgdsCore.cmake`：复用纯核心源文件和现有 JSON 依赖的构建函数。
- `scripts/test-vgds.ps1`：不经过 vcpkg toolchain、不查找 OSG/GDAL/Qt 的独立构建测试入口。
- 本文档。

修改：根 `CMakeLists.txt` 增加 `VEYLAXIS_VGDS_ONLY` 早期分支和正常 Test 的核心测试；`tests/main.cpp` 增加 `--vgds`；README 增加入口说明。主程序业务源码、天空/海洋、海图算法不变。正常解决方案仍为 Veylaxis 和 Test；纯核心配置只生成一个独立 Test。

## 核心接口

```cpp
Result<TileID> locate(const GridSpec&, XY position);
Result<Bounds> boundsOf(const GridSpec&, const TileID&);
Result<uint32_t> nodeCount(const GridSpec&, double spacingMeters);
Result<vector<string>> normalizeLayers(const vector<string>&);

class ITileProvider {
public:
    virtual ~ITileProvider() = default;
    virtual TileResult request(const TileRequest&) = 0;
};

Result<string> encodeRequestJson(const TileRequest&);
Result<TileRequest> decodeRequestJson(const string&);
Result<string> encodeJson(const TilePackage&);
TileResult decodeJson(const string&);
```

这里 `Result<T>` 为 `variant<T, TileError>`，对应 C++17；接口不抛出用于普通请求/解码失败的业务异常。MockProvider 构造时若网格配置非法，会抛出 invalid_argument，运行时请求返回类型化错误。

公共头只包含标准库类型，无 OSG、GDAL、Qt 和 JSON 库类型。nlohmann_json 3.12.0 仅作为编解码及测试实现依赖，使用本机已安装版本，未下载或安装新包。没有为了每个源码职责创建额外库项目；构建函数可把同一组纯核心源文件编入调用目标。

## 网格与空间语义

`GridSpec` 包含 id、horizontalCrs、origin、tileSizeMeters、horizontalUnit。没有业务默认原点或固定 EPSG；用户给定的 EPSG:32619、(503500,4856500)、1000 m 仅在测试中配置，还测试了不同原点、250 m 边长和另一 CRS 标识。

核心只接受单位字段 `m`。CRS 标识是透明字符串，**不调用 GIS 库解析 CRS、确认 EPSG 是否为投影坐标或执行投影转换**；调用方必须提供米制投影/局部坐标。实际数据适配器的 CRS 检查属于后续阶段。调用方也应保证 gridId 在网格定义变更时更新，不将不同原点/尺寸注册到同一逻辑版本。

点对象归属为 `[minX,maxX) × [minY,maxY)`，索引用 floor，支持负索引。恰好落在东北角的点归东北块；线和面允许保留公共边界。定位拒绝非有限坐标、int64 溢出以及 double 无法区分相邻范围的极端坐标。

规则高程为共享节点布局：边长/间距必须为整数，节点数为商+1。1000 m、10 m 对应 101×101，5 m 对应 201×201。当前每边最多 1024 节点，这是明确的 S1 资源上限。

`TerrainData` 行优先：列向 +X，行向 -Y，northWestSample 为第一个样本的全局坐标；Z 向上且单位米，不表示正向水深，不隐含垂直平移。必须提供 verticalDatum、provenance 和有效性掩码。

## 几何、边界与身份

几何支持 Point、LineString、Polygon（外环和孔洞）、MultiPolygon。均使用 double XY、全局米制坐标，没有从二维轮廓暗中补出真实高程。

Ring 隐式闭合，不重复最后一个点，N 个点必须有 N 个边界标记；edges[i] 对应点 i 到下一点。LineString 的 N 个点对应 N-1 个标记。

Schema 1 的边界类型：0=Unknown、1=Geographic、2=ArtificialClip。Geographic 表示来源几何本身的地理边界，不意味着实测；模拟来源必须另标 Synthetic。人工分块线不能在协议中变成真实海岸线。

Feature 保留 layer、objectType、sourceObjectId、fragmentId、dataVersion、provenance、geometry、properties；包内不允许重复 fragmentId。S1 不计算 S57 LNAM、几何裁片身份或裁剪边界，这些由后续适配器和裁剪实现填写。

属性支持 null、bool、int64、有限 double、UTF-8 string、string list，编码中保留属性类型标签，避免整数身份被误当浮点。结构验证检查尺寸、边数、位置范围和身份；不声称已验证自相交、孔洞拓扑或实现 Polygon 布尔求交。

## 请求、响应与 Mock

请求与响应均带 schemaVersion=1，未知版本被拒绝。请求图层使用规范化的小写 ASCII 标识，排序去重；未知但合法的图层标识可以保留并返回 Unsupported。

MockProvider 由 GridSpec 构造，无 OSG/GDAL、全局网格常量、读文件或联网行为。每次请求返回独立值对象：

| 请求图层 | S1 固定行为 |
| --- | --- |
| land / coastline | Empty，没有要素；不是失败，也不是实际岛屿裁剪 |
| terrain | 指定节点布局，固定 -10 m Synthetic 测试数据，中心一个样本故意缺测；不是重建算法 |
| navigation_aids | NoCoverage，验证无覆盖状态 |
| 其他合法图层 | Unsupported，验证扩展/未支持状态 |

Available、Empty、NoCoverage、Unsupported 逐层保存；一个成功 TilePackage 可以包含不支持的图层。非法网格、请求格式或相关分辨率返回 TileError。未请求的图层不返回；Available 的要素层需要实际要素，不能用空数组冒充。

## 最小序列化

采用 UTF-8 JSON 文本，既编码 TileRequest，也编码 TilePackage。请求编码会规范化图层集合。类型、ID、版本、CRS、边界角色、孔洞、来源、掩码和 Float32 高程可往返。

有效高程必须为有限 Float32；未知样本始终由 valid=0 标识，JSON 中必须为 null，解码后为 quiet NaN。编码器会忽略无效槽位中的数字并规范化为 null；消费者不得把无效槽位作为 0 m。JSON 不输出 NaN/Infinity 数字。

拒绝：缺字段、非法枚举、重复 JSON 键、分数/越界 TileID、无效掩码、有效样本为 null、无效样本为数字、Float32 溢出、尺寸与数组长度不符、边界标记缺失、重复片段、包范围与网格不一致等。

当前 JSON 上限 16 MiB、嵌套深度 32，字符串、几何数组和属性数量亦有限制。这是最小本地数据契约，不是经过生产级网络安全/吞吐验证的服务。**未实现二进制数组封装、压缩、HTTP 或错误结果的线格式**；JSON 数字文本不涉及大小端，未来二进制格式需要另行确认。

## 构建与验证

```powershell
cd C:\CProj\Veylaxis

# 独立核心：只指定已有 JSON CMake 包，不使用渲染 SDK 或 GIS toolchain
.\scripts\test-vgds.ps1

# 可手动在已有构建中运行核心测试
.\build\vgds-core\Release\Test.exe

# 常规主程序和 Test 构建及回归
.\scripts\build.ps1
.\scripts\test.ps1
```

纯核心配置等价于：

```powershell
cmake -S . -B build/vgds-core -G 'Visual Studio 18 2026' -A x64 -DVEYLAXIS_VGDS_ONLY=ON -Dnlohmann_json_DIR=C:/CProj/vcpkg/installed/x64-windows/share/nlohmann_json
cmake --build build/vgds-core --config Release
ctest --test-dir build/vgds-core -C Release --output-on-failure
```

纯核心配置在 OSG/GDAL/Qt 查找前返回，不写回根目录 Veylaxis.slnx。CMakeCache 检查未出现这些 SDK 的查找项。dumpbin 检查核心 Test.exe 仅依赖 Windows/MSVC 运行库，没有 OSG、GDAL、Qt、SilverLining/Triton DLL。

测试内容包括：两套可配置网格、正负边界和 nextafter 邻点、121 个区块的定位和共享边界、5/10 m 节点布局、请求/图层规范化、Mock 确定性、邻块测试样本一致、四种几何及孔洞/边界分类/属性/身份往返、无效值及异常输入。

最终实际结果：独立 Release 配置 CTest 1/1 通过（401 项断言检查，CTest 总耗时约 0.19 s）；常规 Release 主程序及 Test 构建成功，CTest 6/6 通过（约 46.45 s），其中包含原有五项功能回归。测试不会创建 OSG 窗口或启动服务。上述耗时是本次执行记录，不是冷/热性能基准。

独立日志为 `build/vgds-core/Testing/Temporary/LastTest.log`，常规日志为 `build/Testing/Temporary/LastTest.log`。二进制依赖检查已实际执行；公共头扫描未发现第三方类型，核心源码中也未出现当前测试 EPSG 和原点常量。

## 未解决事项与阶段边界

- S1 没有加载 80 点岛屿；land 为空是显式 Mock 行为。S2 再迁移 fixture、验证通用裁剪、来源身份和矩形边界。
- 不解析真实 CRS、不选择真实陆地区域、不读取 S57。来源对象 ID 目前是预留字符串约定。
- 无异步请求、取消、缓存、任务状态、TileManager 和网络端口。同步 Provider 仅提供核心契约。
- 尚未定义二进制包和生产级性能指标；JSON 只是可测试的最小编码。
- 本阶段没有修复既有海底重建内部台阶，也没有改变既有栅格和模型的语义。

S1 完成后暂停，等待用户确认是否进入 S2。
