# 代码排版说明

项目自有 C++ 代码统一使用根目录 `.clang-format`。本次处理 `src`、`include`、`tests` 下的 72 个 `.cpp`、`.hpp`、`.h` 文件，不处理第三方库、生成工程、输入数据或配置文件。

排版约定：

- 使用 4 个空格缩进，不使用制表符。
- 普通语句按分号分行，展开同一行上的多个声明或操作。
- 函数、类和控制块的大括号另起一行。
- 展开短函数、条件、循环及 lambda 的函数体，方便逐行阅读和设置断点。
- 长表达式按约 110 列换行，保留字符串字面量，不机械拆分字符串。
- 保留 include 顺序、中文注释内容及命名，不自动添加或删除控制块大括号。
- `for (初始化; 条件; 递增)` 的分号属于循环头，继续保留在循环头内。

同一条声明中的多个变量，例如 `double x{}, y{};`，仍可保留在一起。本次目标是展开语句和结构，不改写声明或算法。

使用本机 Visual Studio 已安装的 clang-format 22.1.3，没有安装新的项目依赖。以后编辑单个文件时可以执行：

```powershell
& 'C:/Program Files/Microsoft Visual Studio/18/Insiders/VC/Tools/Llvm/x64/bin/clang-format.exe' -i src/streaming/TileManager.cpp
```

只检查排版而不修改文件：

```powershell
& 'C:/Program Files/Microsoft Visual Studio/18/Insiders/VC/Tools/Llvm/x64/bin/clang-format.exe' --dry-run --Werror src/streaming/TileManager.cpp
```

其他机器可使用其安装位置下的 clang-format，规则仍取项目根目录文件。

格式化前后逐文件比较有效 C++ token（保留字符串与字符字面量，忽略空白和注释），72 个文件全部一致。源码由 5132 行展开为 12069 行，未改变业务逻辑。文件清单、行数和 token 哈希在 `output/vgds/formatting-validation.json`。原阅读指南按函数名导航，格式化后仍适用。

2026-10-29 验证：72 个文件的 clang-format 排版检查全部通过；Release 下 Veylaxis、VgdsService、Test 重新构建成功；14 项回归测试全部通过，耗时 21.51 秒。包含数据核心、服务、S-57、Catalog、动态区块、岛屿、海底、真实 DMU 及区域 001 栅格/模型检查，本次未重复执行耗时的 `vgds_acceptance` 路线验收。测试日志为 `build/formatting-regression.log`。
