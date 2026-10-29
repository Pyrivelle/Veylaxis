# DMU 航标与船只加载

模型渲染入口位于 `src/streaming/ModelRegistry.cpp`，支持 `.osgb`、`.dmu3D` 和 `.dmuShip`，扩展名不区分大小写。DMU 使用用户指定的 `osgDB::Options（读取环境变量 DMU_READER_OPTIONS）`；航标与运动船只共用此入口及文件资源缓存。VGDS Core 和静态岛屿 OSGB 构建保持不变。

`osgdb_dmu.dll` 从用户提供的 BasicEarth 插件目录复制到 `plugins/osgPlugins-3.6.5/osgdb_dmu.dll`。构建 Veylaxis / Test 时复制到其可执行文件旁的 `osgPlugins-3.6.5`。首次读取 DMU 时显式加载该 DLL，并注册 `dmu3d`、`dmuship` 到 `dmu` 的扩展名别名；优先找可执行文件旁的插件，开发环境可回退项目插件目录。不需要将 BasicEarth 加入系统 PATH。

配置仍为 `config/navigation_models.json`。已有航标选择不变，将其 `.bmp` 预览图路径改为同目录存在的 `.dmu3D`。船只配置新增：

```json
"vesselModelPath": "D:/Dragon/_Data/02/020152/020152.dmuShip"
```

Viewer 原来的船只小方块已经删除，用该配置加载的模型作为运动节点的子节点，沿用位置更新和相机跟随。模型保持原始比例与原点，没有为了展示自动居中、缩放或猜测水线。航标仍可使用各规则的 scale / rotation / heightOffset。读取失败告警，不再用方块伪装成功。

启动：`scripts/view-vgds.ps1`。替换船只或航标仅需修改 JSON，不必改 C++。

新增 `Test --dmu-models` / `dmu_models` CTest，实际读取配置中的船只及所有非空航标路径，检查模型有效包围球、资源复用、真实航标实例化和实例释放。外部船只文件不存在时该集成测试返回 77 并被 CTest 标记跳过；原有模拟测试不依赖 Dragon 资产。外部资产未复制进项目，部署到其他机器仍需修改配置或提供原资源。

此 DLL 为用户提供的二进制插件，当前验证目标是本机 x64 Release。Debug 或其他 OSG ABI 环境需要配套插件，不能假定此 DLL 跨版本兼容。未修改天空、海洋或海底模块。

插件还导入 osgSim / osgFX / osgTerrain / osgVolume 及 zlib.dll。构建步骤部署这些现有 vcpkg 库；本机 zlib 的文件名为 z.dll，确认插件所需 inflateInit2_ / inflate / inflateEnd 导出存在后，将其以兼容名称 zlib.dll 部署到程序旁，未下载新依赖。运行进程已核实实际使用的是项目 build/Release 下的 DMU 插件及 zlib.dll。

2026-10-29 Release 验证：构建成功，6 项相关 CTest 全部通过（dmu_models、navigation_models、vgds_streaming、vgds_core、vgds_service、vgds_s57），总计 6.24 秒。DMU 专项 15 项检查通过，真实船只及 10 个航标加载成功，船只包围球半径约 37.33 米。结果见 `output/vgds/dmu-model-validation.json` 和 `build/dmu-regression.log`。海洋融合 Viewer 截图见 `output/vgds/dmu-smoke.png`。
