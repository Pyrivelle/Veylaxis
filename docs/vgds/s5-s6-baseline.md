# S5、S6 基线启动

2026-10-29。新增 Bathymetry.hpp/.cpp、BathymetryReader.hpp/.cpp、SeabedMaritimeTests.cpp；扩展 S57Options、Dataset、LocalService、CatalogProvider、OsgBuilder 和主程序加载器。未改动天空/海洋算法，没有下载新依赖。

## S5 海底

一次图幅读取 DEPARE、DEPCNT/VALDCO、SOUNDG Point/MultiPoint Z；保留明确 DSPM_SDAT，检查 DSPM_DUNI=米。读取无效范围或缺字段的深度记录会计数，不用 0 填充。深度向下为正，输出高程 z=-depth。

沿用已有 ISeabedReconstructor/LinearContourReconstructor。每个 DEPARE 仅关联真正共边、深度等于下/上界的等深线段；端点按 1 mm 量化匹配，生成相邻线线性坡面，再使用该 DEPARE 内的 SOUNDG 残差约束。按来源 ID 固定选择重叠 DEPARE。Prepared Geometry 加速水域包含判定，避免逐点遍历复杂孔洞；没有资料或无可用相邻线时保留 NoData。

采样在全局 GridSpec 对齐的共享节点上进行，1000 m / 10 m 得到 101×101。没有每块独立归一化。法线也查询全局场的相邻点；新增 TerrainData.normals 为可选纯 STL 数组，JSON schema 1 的旧包仍可读取，旧客户端可忽略此字段。无效高程仍为 mask=0、JSON null、内存 NaN。

Catalog 在每个采样点选择覆盖该点的最高优先级图幅，公共 Tile 节点使用同一选择规则，不按块中心选图。不同有效 sounding datum 同时出现在一个 Tile 时拒绝混合，要求显式垂直转换；不把不同基准悄悄统一到海平面。绘制只使用有效节点组成的三角形，未知区域留空。

当前 US2EC04M 实测读取 369 个 DEPARE、386 个 DEPCNT、3088 个测深点，拒绝深度记录为 0。原海底实验区测试得到 10201 个有效节点；相邻两块的 101 个公共边界节点高度和法线一致。合成案例验证线性坡面、SOUNDG 约束和 DEPARE 遮罩。NoData、法线 JSON 往返和错误法线拒绝均通过。

这是 Estimated 插值海底，不是实测 DEM。当前启动基线未迁移旧 CLI 中的矛盾 DEPARE 条带修复，原始约束的内部跳变/明暗条带仍可能出现。共享 Tile 节点一致不等于整个 ENC 内部已经消除所有约束矛盾。10 m 是渲染采样间隔，不代表原始海图精度；海洋动画的 Z=0 与海图基准尚无潮位转换。

## S6 海事图层

| 请求图层 | 已读取对象类 |
| --- | --- |
| fairway | FAIRWY |
| dredged_areas | DRGARE |
| navigation_aids | LIGHTS、BOYLAT、BOYCAR、BOYISD、BOYSAW、BOYSPP、BCNLAT、BCNCAR、BCNISD、BCNSAW、BCNSPP |

保留对象类、cell+LNAM、RCID/RVER、原始属性及数据版本。面/线裁剪保持来源身份和边界角色，点按半开 Tile 区间和 Catalog 图幅优先级唯一分配。已支持类在当前海图中没有对象时返回 Empty；未知图层 Unsupported；海图外 NoCoverage。

US2EC04M 中实际有 FAIRWY=2、LIGHTS=157、BOYLAT=14，以及其他航标类；没有 DRGARE，测试不虚构疏浚区。NAV 请求只返回 NAV 数据，保留位置和属性。静态 OSGB 不包含航标模型；TileLoader 在主程序侧用黄色锥体作示意标记，随区块卸载。真实模型资源、按属性选型、灯光和航标动画尚未实现。

两阶段联合测试首轮 423 项检查通过，机器记录 output/vgds/s5-s6-validation.json。测试命令：Test.exe --seabed-maritime。海底自动随 terrain 图层请求返回；本轮没有额外强制导出 GeoTIFF，原 reconstruct 命令仍可导出旧区域的地理参考 TIFF。
