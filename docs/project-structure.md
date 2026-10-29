# 解决方案组织说明

## VGDS S2–S4 后的当前结构

2026-10-29 按新增的“独立 OSGB Builder”设计扩展为五个构建目标：Veylaxis、Test、VgdsService，以及 VgdsData、可选 VgdsOsgBuilder 两个静态库。没有为每个类或图层创建工程。

VgdsService 是本地 JSONL 请求入口，VgdsData 供服务、主程序与测试共享 Core/裁剪/缓存/适配代码，GDAL 仅留在实现中；VgdsOsgBuilder 隔离 OSG 文件输出依赖。纯 STL Core 仍通过 VGDS_ONLY 配置单独编译，不因共享数据层含适配器而依赖 GDAL。OSGB=OFF 的 SERVICE_ONLY 配置不查找 OSG、Qt 或海洋 SDK。

相较于此前只有一个应用原型，现在确实有服务与主程序两个消费者，以及必须隔离的 OSG 依赖边界，因此新增这两个共享库有具体用途。其他源码职责继续用目录/文件区分。根 Veylaxis.slnx 列出这些目标，不列 CMake 辅助入口。

## 先前两项目结构（历史记录）

在 Visual Studio 打开根目录的 `Veylaxis.slnx`，解决方案中只有两个项目：

```text
Veylaxis 解决方案
├── Veylaxis → Veylaxis.exe
│   ├── main.cpp             统一入口
│   ├── bathymetry.cpp       S57 读取与栅格生成
│   ├── reconstruction.cpp  可替换的重建算法
│   └── viewer.cpp           OSG 地形显示与导出
└── Test → Test.exe
    ├── main.cpp             测试入口
    ├── reconstruction_tests.cpp
    └── raster_check.cpp
```

`Test` 项目直接编译同一份 `src/reconstruction.cpp`，因此测试的仍然是实际算法代码。源文件没有复制，也没有单独的算法库工程。当前只有一份很小的共享算法实现，在两个可执行文件中各编译一次，代价可以接受。

这里的“主项目”和“测试项目”是 Visual Studio 解决方案里的两个项目，不是 Git 分支。Git 分支用于保留不同开发版本，例如稳定版本 main 和某项功能的开发版本。测试代码通常和它所测试的业务代码放在同一个 Git 分支，便于同时修改与验证。当前目录尚未初始化 Git，本次没有创建 Git 分支。

## 原来为什么有那么多项目

原来有两种不同来源的项目：

| 原项目 | 用途 | 本次处理 |
| --- | --- | --- |
| veylaxis_bathy | 独立的海底重建程序 | 合入 Veylaxis 的 reconstruct 功能 |
| veylaxis_viewer | 独立的 OSG 预览程序 | 合入 Veylaxis 的 view 功能 |
| veylaxis_terrain | 供主程序与测试复用的算法静态库 | 改为直接编译同一份源文件 |
| reconstruction_tests / raster_check | 两个单独的测试程序 | 合并成一个 Test 程序 |
| ALL_BUILD / ZERO_CHECK / RUN_TESTS | CMake 构建、配置检查和测试入口 | 从用户入口解决方案中排除 |
| Continuous / Experimental / Nightly | CTest/CDash 持续集成辅助任务 | 不再生成这些任务 |

把模块做成独立库，适用于多个程序复用、独立发布、插件加载、不同依赖边界或大型团队并行维护。例如以后同一套重建算法同时供桌面程序和服务器使用，再单独抽出库就很合理。当前只有一个应用原型，拆成多个项目增加了启动和理解成本，收益有限，因此改成主程序与测试程序各一个。

代码分文件、通过接口解耦，和在解决方案中拆成多个项目是两回事。现在继续保留读取、算法和显示的源文件分工，避免把所有逻辑塞进一个巨大 main.cpp；但它们共同编译为一个 Veylaxis.exe。

## 使用方式

```powershell
# 首次生成或修改 CMake/源文件清单后执行
.\scripts\build.ps1

# 重建默认区域
.\scripts\run-region001.ps1

# 打开 OSG 窗口
.\scripts\view-region001.ps1

# 执行全部五项验证
.\scripts\test.ps1
```

直接启动命令：

```powershell
. .\scripts\runtime.ps1
.\build\Release\Veylaxis.exe reconstruct
.\build\Release\Veylaxis.exe reconstruct --help
.\build\Release\Veylaxis.exe view output/region001/region001_elevation.tif
.\build\Release\Test.exe
.\build\Release\Test.exe --raster output/region001/region001_elevation.tif
```

`Test.exe` 不带参数时执行算法测试；`--raster` 校验已有栅格；`--scene` 核对导出模型比例。完整验证仍由测试脚本调度，五项测试共用一个 Test 项目。

Visual Studio 的工作目录和 GDAL、PROJ、OSG 调试环境已配置。将 Veylaxis 设为启动项目，不带参数按 F5 会打开区域 001 融合场景；也可以显式填写 `view output/region001/region001_elevation.tif`。重新生成海底使用 `scripts/run-region001.ps1`。将 Test 设为启动项目可直接调试算法测试。

根目录的入口解决方案在 CMake 配置时生成。`build/Veylaxis.slnx` 是 CMake 内部生成的构建解决方案，可能含 ALL_BUILD、RUN_TESTS 辅助项目；日常使用打开根目录的同名文件即可。关闭了自动重新配置项目，新增源文件或修改 CMakeLists.txt 后运行构建脚本重新生成；修改已有 C++ 文件仍可直接在 Visual Studio 编译。

海图处理算法、栅格参数、质量标记和输出文件格式保持现有行为，之前生成的区域成果仍可使用。
