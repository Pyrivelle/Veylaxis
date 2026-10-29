# Veylaxis

首次克隆请先阅读 [本机环境配置](docs/local-environment.md)，复制 `.env.example` 为 `.env` 并填写本机授权及依赖路径；密钥、海图、模型和生成产物不入库。

**首次阅读项目，请从 [项目阅读指南](docs/project-reading-guide.md) 开始。** 文档按入口、数据契约、异步区块、真实海图、算法和渲染排列阅读步骤；关键源码已补充中文导航注释。

S-57 航标可通过 `config/navigation_models.json` 配置 OSGB / DMU 模型，运动船只使用同文件的 `vesselModelPath`。DMU 插件部署及真实模型验证见 [DMU 模型加载](docs/vgds/dmu-model-loading.md)。使用 `scripts/view-vgds.ps1 -NavigationModels config/navigation_models.json` 启动；分类、坐标、缓存及限制见 [航标模型映射](docs/vgds/navigation-model-mapping.md)。

VGDS 已提供纯 STL 数据核心、本地 JSONL 服务、带孔/MultiPolygon 跨块裁剪、可选 OSGB 缓存和主程序 3×3 异步加载。模拟岛屿和真实 S-57 LNDARE 共用请求及场景接口。运行 `scripts/test-vgds.ps1` 继续独立测试纯核心，无 OSG/GDAL/Qt；实现记录见 [S1](docs/vgds/s1-core.md)、[S2](docs/vgds/s2-service.md)、[S3](docs/vgds/s3-streaming.md)、[S4](docs/vgds/s4-s57.md)。

本轮交付与测试总表：[S2–S4 最终验收](docs/vgds/s2-s4-results.md)。

S-57 海图到海底高程栅格的 C++17 / GDAL / OpenSceneGraph 实验工程。首个区域为 `US2EC04M.000` 中的实验区域 001。

Visual Studio 请打开根目录的 **`Veylaxis.slnx`**。保留 `Veylaxis` 主程序和 `Test`，本轮增加 `VgdsService` 入口、共享的 `VgdsData` 数据层静态库及可选 `VgdsOsgBuilder` 静态库，没有按每个类拆项目。首次使用前先运行构建脚本。历史组织说明见 [解决方案组织说明](docs/project-structure.md)。

统一主程序为 `Veylaxis.exe`：`vgds` 进入动态海图区块场景，`reconstruct` 执行离线海底重建，`view` 执行 OSG 预览或导出。正常构建不带参数时使用 `config/vgds-catalog.json` 打开动态融合场景。

正式 Viewer 已融合来自 PyriveMarine 的 **SilverLining 天空、云层和 Triton 动态海洋**，海底与环境统一为 **XYZ 1:1 米制坐标**。按 F1 查看海面，F2 切换水下，O 隐藏/显示海面。独立海底夸张检查用 `scripts/view-region001.ps1 -SeabedOnly -ZScale 10`。见 [融合说明](docs/marine-integration.md)。

已生成的成果位于 `output/region001`，核心输出为 `region001_elevation.tif`。算法、数据局限与验收结果见 [区域 001 实验记录](docs/region001.md)，构建与扩展方式见 [技术文档](docs/technical-design.md)。

九块独立重建与拼接实验已完成，成果位于 `output/tile-test/halo250_normals`，运行 `scripts/view-tiles.ps1` 可查看。测试过程、高程与法线接缝、耗时及保留的地形质量问题见 [九块测试报告](docs/tile-test-results.md)。

在项目根目录的 PowerShell 中执行：

```powershell
.\scripts\build.ps1 -VcpkgRoot C:\CProj\vcpkg
.\scripts\run-region001.ps1
.\scripts\test.ps1
.\scripts\view-region001.ps1
```

仅导出 OSG 场景、不打开窗口：

```powershell
.\scripts\view-region001.ps1 -ExportOnly
```

当前使用本机现有 vcpkg 库，无需调用 QGIS 的 GDAL，也无需修改系统 PATH。运行脚本从 CMake 缓存取得实际使用的 vcpkg 根目录，并在当前进程中配置 DLL、GDAL 数据、PROJ 数据与 OSG 插件路径。

查看 VGDS 动态区块（默认使用现有天空、海洋；`-LandOnly` 只看陆地）：

```powershell
.\scripts\view-vgds.ps1 -Tour
.\scripts\view-vgds.ps1 -Config config/vgds-s57.json -Tour -LandOnly
```

当前默认开启海岸距离模拟岛屿高度场，配置高度上限为 42 m，XYZ 比例为 1:1，不是实测陆地高程；关闭该高度场时保留旧 12 m 平顶分支。`osgb:false` 关闭文件输出，仍可从语义数据内存建模；OSGB 不替代原始地理数据。

区域 001 预览运行后生成于 `output/region001/region001_preview.png`，本机产物不入库。
