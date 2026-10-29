# SilverLining、Triton 与海底融合

## 启动

本阶段将 `C:\CProj\PyriveMarine\src\environment` 的 SilverLiningSkyBox 和 TritonOceanSurface 适配代码复制到本工程，保留单一 Veylaxis 主项目。未改写 PyriveMarine 工程。Qt 仅提供原适配代码使用的 Core/Gui 类型，不引入其 Qt 主窗口；继续使用现有 OSG Viewer。

```powershell
.\scripts\build.ps1
.\scripts\view-region001.ps1
```

Visual Studio 使用 **Release | x64** 或 **RelWithDebInfo | x64**，SDK 来自现有 Release 二进制库。F5 无参数默认打开融合场景；重新生成海底栅格仍使用 `scripts/run-region001.ps1`。

| 按键/操作 | 功能 |
| --- | --- |
| F1 | 海面视角 |
| F2 | 水下视角 |
| O | 显示/隐藏海面，便于检查海底 |
| 左键拖动 | 旋转 |
| 中键拖动 | 平移 |
| 滚轮 | 缩放 |
| 空格 | 恢复当前海面/水下预设视角 |
| W | 海底线框 |

## 坐标和尺度

正式融合场景中 XYZ 一单位均为一米，Z 向上，**Z 缩放为 1**。海底位置来自 EPSG:32619 栅格，XY 使用栅格中心作为局部原点，全球投影坐标和 CRS 继续保存在模型元数据中。

海面平均水位放在 Z=0，海底使用 `Z=-depth`。这里将海图水深基准作为演示海面零位，没有做实时潮位或海图基准到平均海面的换算。Triton 海浪在零位附近动态起伏，海底模型本身没有被缩放或抬升。

SilverLining 使用栅格中心转换得到的位置，约 `(-68.950216, 43.866005)`。演示时间设为系统日期的 UTC 16:00，用于当地白天光照；没有复刻实时天气。海洋默认风速为 8 m/s，天空的太阳光方向、直射光和环境光逐帧传给海洋。

独立海底调试仍可使用垂直夸张：

```powershell
.\scripts\view-region001.ps1 -SeabedOnly -ZScale 10
```

融合模式会拒绝非 1 倍 Z，避免海面与海底采用不同尺度。`-ExportOnly` 继续导出普通的海底 OSG 网格，默认保持 1:1；SDK 天空和海洋需要运行时初始化，不作为静态网格写进海底模型文件。

## 依赖与部署

新增依赖来自现有本地安装，无需下载安装：

| 依赖 | CMake 配置 |
| --- | --- |
| SilverLining / Triton | `VEYLAXIS_SDK_ROOT`，默认 `D:/SDK` |
| Qt Core / Gui | `VEYLAXIS_QT_ROOT`，默认 `D:/Qt/Qt5.14.2/5.14.2/msvc2017_64` |
| OSG、GDAL、GEOS、PROJ | 继续使用当前 vcpkg 配套 |

`cmake/DeployMarine.cmake` 在主程序构建后部署 Qt DLL、天空/海洋资源以及 SilverLining OpenGL 后端。Triton 配置采用内置 FFTSS CPU 波浪计算后端，免去旧 CUDA/OpenCL/IPP 后端及其大型运行库；海洋仍由 Triton 渲染和模拟。原 SDK 的全局配置不变。

SDK 密钥现在统一从根目录 `.env` 或进程环境读取，模板为 `.env.example`，详见 [本机环境](local-environment.md)。不再读取或部署旧 INI。

运行目录下必须保留：

```text
build/Release/
├── Veylaxis.exe
├── Qt5Core.dll / Qt5Gui.dll
├── .env（自行提供，不随构建复制）
└── resource/
    ├── SilverLining/resources/
    │   ├── SilverLining.config
    │   └── VC14/x64/SilverLiningOpenGL-MT-DLL.dll
    └── Triton/Resources/Triton.config 及纹理、着色器
```

SilverLining 根据其 ResourceLoader 的资源路径查找渲染后端，不能只把后端 DLL 放在 exe 旁边。首次融合时这一级目录缺失，导致天空初始化返回 `E_CANT_LOAD_RENDERER_DLL`。原云层代码在初始化失败后仍可能调用绘制，造成访问异常；现在保留资源目录层级，并增加前置检查、失败状态判断与安全退出。海洋初始化失败也会停止场景，不再显示为空白成功。

## 场景与渲染

`MarineEnvironment` 负责资源和本机配置、创建天空/海洋节点、传递光照及清理 SDK。`src/viewer.cpp` 创建统一场景根节点，下面同时挂接海底、天空、海洋。模型、海洋、天空、云层分别使用 1、10、98、99 号渲染队列，保留源工程对 OpenGL 状态缓存的恢复处理。

相机采用固定近远裁剪面，避免无限海洋与天空包围盒把海底的深度精度拉坏。初始观察位置只依据区域的局部米制范围，不使用包含天空/海洋的大包围盒计算视距。

相机采用 `ZUpManipulator`，固定世界 Z 向上，鼠标旋转只改变方位和俯仰，不允许横滚，并关闭松开鼠标后的惯性旋转。这锁定的是朝向参考轴，不是相机高度；F2 仍可将相机放在 Z=-12 m 的水下。相机回归测试执行 400 次不同方向的拖动，验证右方向保持水平、上方向不会翻转，以及水下位置不会被抬升。

## 海底为什么在水下

核对源栅格、网格生成公式和融合节点后，确认不是额外竖直偏移：

| 对象 | Z 值/约定 |
| --- | --- |
| Triton 平均海面 | 0 m，海浪在其附近动态起伏 |
| 当前海底最浅像元 | -1.800315 m |
| 当前海底最深像元 | -36.854630 m |
| 海底 Z 缩放 / 平移 | 1 / 0 m |

海图的水深向下为正，模型采用 Z 向上，因此 `Z=-水深`。例如一个 1.8 m 测深点在模型中的精确位置应为 Z=-1.8 m。XY 为避免大坐标精度损失而减去 UTM 局部原点，Z 没有减去局部原点或额外移动。把海底抬到海面之上会破坏当前水深关系，故不做这种调整。

海面零位目前对应演示采用的海图水深基准，仍不等于已经完成真实潮位改正；该区别不改变海底负 Z 的正确符号。

本阶段是视觉场景融合：没有把海底栅格接入 Triton 高度图，也没有添加浅水波变形、潮汐或海底碰撞物理。F2 可进入水下观察海底，O 可关闭海面直接核对地形。

## 验证

原有五项 CTest 继续检查插值、真实栅格、区域约束、模型导出和逐顶点 XYZ 1:1 比例。

新增实际 OpenGL 渲染测试：

```powershell
.\scripts\test-marine.ps1
```

该模式等待 SDK 就绪，分别渲染海面和水下视角，保存以下截图并正常关闭，失败返回非零值：

- `output/region001/region001_marine.png`
- `output/region001/region001_marine-underwater.png`

首次补齐渲染后端后，已确认 `sky=1 ocean=1 terrain=1 XYZ_scale=1`，两个视角成功保存且程序正常退出。最终验证结果以本次实际执行日志为准。

![融合海面场景](../output/region001/region001_marine.png)

![水下海底视角](../output/region001/region001_marine-underwater.png)
