# S-57 航标到 OSGB 模型映射

后续 DMU 改造已支持 `.dmu3D` / `.dmuShip`，并将用户填写的 10 个预览图路径换为对应模型，增加真实运动船只配置。当前使用与部署说明见 [DMU 模型加载](dmu-model-loading.md)；下文默认空路径是初版模板说明。

## 使用

编辑根目录 `config/navigation_models.json`，给需要显示的类别填写 `modelPath`。默认 14 项均为空，未提供模型不会生成替代圆锥。路径不进入 VGDS 数据包或服务端配置。

```json
{
  "version": 1,
  "maxCachedModels": 64,
  "mappings": [
    {
      "id": "port",
      "objectClass": "BOYLAT",
      "attribute": "CATLAM",
      "value": 1,
      "modelPath": "../models/port.osgb",
      "scale": 1,
      "rotation": [0, 0, 0],
      "heightOffset": 0
    }
  ]
}
```

此处路径仅是格式示例。允许 Windows 绝对路径（JSON 使用 `/` 或 `\\`）；相对路径以 **JSON 所在目录** 为基准。JSON 使用 UTF-8，支持中文模型文件名。模型路径由用户填写，不需要修改 C++。修改后重启 Viewer 生效。渲染接口也提供 `ModelRegistry::reload()`，失败时保留上次有效规则；已有 Tile 不会自动替换，调用方需要重新加载 Tile。本版没有文件监视和热更新 UI。

```powershell
.\scripts\build.ps1
.\scripts\view-vgds.ps1 -NavigationModels config/navigation_models.json
# 或在项目根目录、配置好 scripts/runtime.ps1 后：
.\build\Release\Veylaxis.exe vgds --config config/vgds-catalog.json --navigation-models config/navigation_models.json
```

VGDS 配置须打开 `maritime:true`，请求图层包含 `navigation_aids`；现有 `vgds-catalog.json` 已启用。换 JSON 不要求重新生成岛屿缓存。

## 分类核对

依据本工程实际使用的 vcpkg GDAL 字典：`C:/CProj/vcpkg/installed/x64-windows/share/gdal/s57objectclasses.csv`、`s57attributes.csv` 和 `s57expectedinput.csv`。

|对象（OBJL）|分类属性|默认映射值|
|---|---|---|
|BOYLAT（17）|CATLAM（36）|1 port-hand / 左侧；2 starboard-hand / 右侧|
|BOYCAR（14）|CATCAM（13）|1 北；2 东；3 南；4 西|
|BOYISD（16）|无分类条件|孤立危险标|
|BOYSAW（18）|无分类条件|安全水域标|
|BOYSPP（19）|默认无分类条件；保留 CATSPM（66）列表|特殊标志|
|LITVES（77）|无分类条件|灯船|
|MORFAC（84）|CATMOR（40）|3 bollard；5 post or pile；7 mooring buoy|
|BERTHS（10）|无分类条件|可选自定义泊位旗|

CATLAM=3、4 是推荐航道标类别，不归为普通左/右侧标。CATMOR=1、2、4、6 不猜成这三种系泊设施。缺失、null、布尔值、未知分类不匹配需要该属性的规则。CATSPM 是列表，保留每一项；配置有属性规则时按列表成员精确匹配。数值 `1`、字符串 `"1"` 可匹配同一个枚举，绝不做字符串子串匹配。

属性规则优先于同对象类的无条件规则；同优先级按 JSON 顺序取首个匹配。可以通过 `enabled:false` 停用规则。id 必须唯一。空路径、缺失或无法读取的模型会告警并跳过；不会以其他类别的模型作替代。可在 BOYSPP 的无条件规则之外增加 CATSPM 条件规则。LIGHTS、BCN* 等之前已提取的类别继续保留，没有默认模型；用户可以自己新增映射。

## 数据链路和生命周期

1. `readS57()` 应用海图更新，以配置的投影转换坐标；新增提取 LITVES、MORFAC、BERTHS，其余浮标类别复用现有解析。
2. 现有 `Feature` 直接携带 `objectType`、`sourceObjectId`、`fragmentId`、`dataVersion` 及原始 OBJL、LNAM、RCID、RVER、分类属性和源文件信息。无需改动 Core schema 或 TilePackage，Core 仍为纯 STL。
3. `LocalService` / `CatalogProvider` 复用既有裁剪及源优先级逻辑。点采用 `[minX,maxX) × [minY,maxY)` 归属；恰在边或角的点只分配给一个 Tile。Catalog 继续按 M_COVR 和比例尺选源，渲染层不另找海图。
4. `TileLoader` 载入静态陆地后，向独立的 `ModelRegistry` 请求航标组。每个点有自己的 MatrixTransform，相同路径共享模型节点，禁止修改共享模型的几何/材质来做单实例动画。
5. 放置坐标为投影点减 Tile 原点，外层仍由 TileManager 平移到场景坐标。XY 和 Z 单位都是米，无额外地形夸张。`scale` 为正标量或三分量数组；`rotation` 为局部 XYZ 依次旋转的角度（度，右手系，Z 向上）；`heightOffset` 为相对场景海面 Z=0 的垂直偏移。没有用海图水深猜测浮标高度或模型原点。
6. 按 `sourceObjectId` 去除一个包内的重复片段，同时再次验证点的 Tile 归属。数据层要求源 ID 唯一，Catalog 排除重叠覆盖，因此同一固定位置对象不会跨相邻 Tile 重复创建。没有对不同海图中不同 LNAM 的现实同一物标做模糊合并。
7. 航标组附属于 Tile。区块卸载或过期任务丢弃会释放实例及变换。模型资源保存在有界 LRU 中，可供返程复用；实例释放不等于立即从资源缓存移除原型。默认最多 64 个路径，可配置 1–256，按条目数限制而非显存字节数。被淘汰模型仍由活动实例持有，最后实例卸载后释放。

加载时检查文件大小及修改时间；未变化模型复用内存。损坏文件也缓存失败状态，文件变化或 reload 后重试。缺失文件会在之后请求时重试存在性。警告按规则或路径去重，避免每帧重复打印。Registry 用互斥锁保护缓存；读取和实例创建发生在现有异步加载线程，场景挂接/卸载继续在更新线程执行。

Provider 适配版本和 Catalog 缓存键包含本次航标类别版本，避免之前缓存缺少新增对象。岛屿算法和 OSGB Builder 代码没有改变，OSGB 仍只是可再生成的静态渲染缓存，导航模型不写入该文件。

## 边界及未实现范围

- MORFAC、BERTHS 字典允许点、线、面。保留这些原始几何；第一版模型实例化只接受点，不猜测线/面中心点，遇到其他几何告警跳过。
- 原始类别属性缺失时保留缺失，不补默认分类。未覆盖或海图不存在对象时正常空返回。
- 用户模型需要自行保证朝向、原点、长度单位，再用 scale/rotation/heightOffset 调整。尚未收到实际业务模型，自动化验证使用临时生成的 OSGB；不能据此确认用户资产的外观和水线。
- 本版不做闪灯、浮动、顶标组合、复杂关联和 IENC 扩展，不依赖现实灯光或潮汐高度。
- 模型更改检测依赖文件大小/时间；完全保持相同元数据的替换请 reload 或重启。

## 修改文件与测试

新增：`include/veylaxis/streaming/ModelRegistry.hpp`、`src/streaming/ModelRegistry.cpp`、`config/navigation_models.json`、`tests/vgds/NavigationModelTests.cpp`、本文。

修改：`src/vgds/S57.cpp`（新增类别及适配版本）、`src/vgds/Catalog.cpp`（缓存键）、`include/veylaxis/streaming/TileManager.hpp`（可选 Registry 参数）、`src/streaming/TileLoader.cpp`（实例接入）、`src/streaming/Viewer.cpp`（配置选项）、`scripts/view-vgds.ps1`（配置选项）、`cmake/VgdsService.cmake`、`CMakeLists.txt`、`tests/main.cpp`、`tests/vgds/ServiceMain.cpp`、`README.md`。

`navigation_models` CTest 覆盖 14 种模拟类别、数值/字符串/列表条件、缺失类别、真实 ENC 来源及独立 GDAL 坐标对照、Catalog 请求和缓存、精确角点归属、源 ID 去重、变换、资源共享/LRU、坏配置、缺失/损坏模型、实际 TileLoader 动态卸载和空覆盖。

2026-10-29，Windows x64 / VS 18 2026 / Release 验证结果：

|验证|结果|
|---|---|
|`scripts/build.ps1` 与最终增量构建|Veylaxis、Test、VgdsService 构建成功|
|主工程完整 CTest|14/14 通过，130.79 秒|
|最终中文路径、真实三维测试模型及共享资源初始化变更后，重跑 `navigation_models`、`vgds_streaming`|2/2 通过，3.94 秒|
|`scripts/test-vgds.ps1`|纯 Core 1/1 通过，无 OSG/GDAL/Qt|
|`scripts/test-vgds-service.ps1 -NoOsgb`|关闭 OSG 的服务构建成功，7/7 通过，19.97 秒|

完整回归日志：`build/navigation-regression.log`；最终专项日志：`build/navigation-targeted.log`；机器可读结果：`output/vgds/navigation-model-validation.json`。测试中的故意损坏 OSGB 会打印预期的 OSG 读取错误，断言验证其被安全跳过。

真实测试海图 `US2EC04M.000` 及 `.001` 更新提取到 BOYLAT=14、BOYISD=2、BOYSAW=8、BOYSPP=42，验证对象为 `US2EC04M:BOYLAT:0226020C9D600032`。独立读取原始 LNAM 对象并用 GDAL 转换经纬度，位置误差要求小于 0.00001 米；该对象也经单海图 Catalog 选源、TilePackage 返回后成功映射到测试 OSGB。此海图未提供的 BOYCAR、LITVES、MORFAC、BERTHS 由模拟源覆盖全部配置分支，不据此声称已在真实数据上逐类验收。既有 Catalog 测试继续覆盖项目内 48 幅 ENC 的多源索引与优先级。

未增加第三方依赖；未修改岛屿 OSGB Builder、海底重建、天空海洋及 perevia-osgkit。构建时仍有既有 `PreparedShape.hpp` 局部变量遮蔽警告，未扩展本次任务去修改该文件。
