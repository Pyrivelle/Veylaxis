# 本机环境与仓库发布

复制根目录 `.env.example` 为 `.env`，填入自己的 SilverLining（Sundog）/Triton 用户名、授权码、DMU_READER_OPTIONS、插件及 SDK 路径。原有本机 INI 密钥已经迁入本机 `.env`，程序不再读取或部署 `runtime.local.ini`。

`DMU_READER_OPTIONS` 是传给 osgDB::Options 的原始字符串，格式为 `password=你的密码`。不要把实际值写入 C++、JSON、文档或提交记录。

PowerShell 运行/构建脚本通过 `scripts/load-env.ps1` 读取根目录 `.env`。直接启动程序时，渲染层的 `loadRuntimeEnvironment()` 读取工作目录或可执行文件所在目录及上层目录中的第一个 `.env`。可用进程变量 `VEYLAXIS_ENV_FILE` 指定文件。已有进程环境变量优先；支持 KEY=VALUE、单/双引号包裹、空行和整行 # 注释，不执行插值、命令或转义。请使用 UTF-8 无 BOM，不使用行尾注释。

模型分类模板在 `config/navigation_models.json`，不含本机模型路径。复制为 `config/navigation_models.local.json` 并填写路径，Viewer 默认优先使用本地文件，启动参数仍可指定其他配置。航标映射结构不变。

`build.ps1` 根据 `.env` 的 DMU_PLUGIN_PATH 复制外部插件到本机 plugins 目录，并使用 SDK/Qt/vcpkg 路径构建。外部 SDK、海图、插件和模型需要自行提供，仓库不包含它们。纯核心测试无需这些专有依赖。

不入库：`.env` 及本地配置、build/output/.vs、S57_DATA、plugins、二进制、模型、地形栅格、压缩产物和机器生成的根解决方案。根 `Veylaxis.slnx` 可通过构建脚本重新生成。仅 `.env.example` 和不含密码的配置模板入库。克隆后先配置本机环境及依赖再构建，真实海图/DMU 测试需要本机资产。

本次发布前检查暂存文件，按实际本机密钥值扫描源码和文档，确认未将敏感文件或大型产物加入首个提交。此项目此前没有 Git 历史，因此使用清理后的内容建立初始提交；没有上传包含旧密码的历史版本。
