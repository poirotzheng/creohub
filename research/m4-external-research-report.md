# creo-hub M4 工程化阶段调研报告（外部产品/技术证据）

> 调研方式：web_search / web_fetch 外部公开资料，优先官方文档、官方新闻稿、GitHub/规范原文。
> 取证原则：只写有来源支撑的结论；来源打不开或未找到的一律标注"未找到公开证据"。

---

## 1. Windows 桌面软件离线安装器选型（Inno Setup vs WiX/MSI vs NSIS；Creo 插件注册先例）

### 结论速览
- **Inno Setup 对"企业内网 + 无管理员权限 + 一键/静默"场景支持最完整**：`PrivilegesRequired=lowest` 原生支持非提权按用户安装，`/VERYSILENT`、`/CURRENTUSER`、`/LOADINF=/SAVEINF=`、`/LOG` 等参数齐备，`[Code]`（Pascal 脚本）可做安装前环境检测与写文件。
- **NSIS** 同样支持静默（`/S`，脚本 `SilentInstall silent`），脚本自由度高，但官方文档年代较老；**MSI/WiX** 走 msiexec（`/quiet` 等价 `/qn`），是企业 GPO/SCCM 分发的事实标准，WiX 还提供 launch condition（环境检测）、QuietExecution 自定义动作、包校验工具与 Burn bundle。
- **PTC 官方"自动注册"机制已经存在且是纯文本**：Creo 通过注册文件 `protk.dat` / `creotk.dat`（逐行关键字-值）描述插件，config.pro 提供 `prodevdat → creotkdat / protkdat / toolkit_registry_file` 选项指向注册文件；另有 GUI 的 Auxiliary Applications（辅助应用）对话框注册/启动。
- 因此安装器的"自动注册"= **写 protk.dat + 写/追加 config.pro 选项**，不需要逆向或私有 API；未找到第三方 Creo 插件安装器做该自动化注册的成熟公开先例（最接近的是 CREOSON 的 CreosonSetup 配置器）。

### 证据条目
- Inno Setup `[Setup]: PrivilegesRequired`（admin/lowest；lowest = 不请求管理员权限、始终非管理员安装模式）——[jrsoftware 官方帮助](https://jrsoftware.org/is6help/topic_setup_privilegesrequired.htm)
- Inno Setup 命令行参数：`/SILENT`、`/VERYSILENT`、`/SUPPRESSMSGBOXES`、`/ALLUSERS`、`/CURRENTUSER`（按 PrivilegesOverridesAllowed 切换安装模式）、`/LOADINF=/SAVEINF=`（保存/回放安装选项，适合批量无人值守）、`/LOG`、`/DIR`、`/COMPONENTS`、`/NORESTART`；同页明确存在 `[Code]` 脚本支持函数（可做环境检测/自定义逻辑）——[jrsoftware：Setup Command-Line Parameters](https://jrsoftware.org/is6help/topic_setupcmdline.htm)
- NSIS 静默安装：脚本指令 `SilentInstall normal|silent|silentlog`，命令行 `/S`（大小写敏感）触发静默——[NSIS Wiki：Reference/SilentInstall](https://nsis.sourceforge.io/Reference/SilentInstall)
- MSI 标准安装器命令行（msiexec）：`/quiet`（等价 `/qn`，无 UI 无提示不可取消）、`/passive`、`/norestart`、`/forcerestart`、`/log`、`/package`、`/uninstall`——[Microsoft Learn：Microsoft Standard Installer command-line options](https://learn.microsoft.com/en-us/windows/win32/msi/standard-installer-command-line-options)
- WiX（生成 MSI 的 XML 工具链）：教程含 "Blocking packages / Launch conditions"（安装前环境检测与阻断安装）、QuietExecution 自定义动作、包 Validation 工具、Burn bundle（引导多个包）、MSBuild 集成——[FireGiant WiX 文档（教程目录与工具页）](https://docs.firegiant.com/wix3/tutorial/getting-started/)
- PTC 官方注册文件格式：`creotk.dat`/`protk.dat` 为逐行关键字-值文本（NAME / TOOLKIT / EXEC_FILE / TEXT_DIR / STARTUP dll|spawn / DELAY_START / ALLOW_STOP / DESCRIPTION / FAIL_TOL / END 等字段，一个文件可含多个应用）——[PTC Creo Object TOOLKIT C++ User's Guide：Registry File](http://support.ptc.com/help/creo_toolkit/otk_cpp_plus/usascii/creo_toolkit/user_guide/Registry_File.html)
- PTC 官方注册文件搜索路径与配置选项：Pro/DEVELOP 的 `prodev.dat` 已由 `creotk.dat`/`protk.dat` 取代，配置文件选项 `prodevdat` 对应 `creotkdat`/`protkdat`/`toolkit_registry_file`——[PTC Creo TOOLKIT User's Guide：Registry Files](https://support.ptc.com/help/creo_toolkit/protoolkit_pma/r11.0/usascii/creo_toolkit/user_guide/Registry_Files.html)
- PTC 官方开发包自带安装示例：Creo TOOLKIT Getting Started 中出现 `protk_install_example` 目录与 `make_install` 目标（官方 makefile 安装示例）；官方 User's Guide 描述 Auxiliary Applications（辅助应用）对话框（Home→Utilities）注册与启动应用——[Creo TOOLKIT Getting Started（PDF，含 make_install/protk_install_example）](https://www.ptc.com/support/-/media/support/refdocs/Creo_Parametric/12/Creo_Toolkit_GSG_Creo12400.pdf?sc_lang=ja-JP)、[Creo TOOLKIT User's Guide（PDF，Auxiliary Applications）](https://www.ptc.com/support/-/media/support/refdocs/Creo_Parametric/8,-d-,0/tkuse_Creo8000.pdf?sc_lang=en)（注：这两条为 PDF 内容片段级证据）
- **未找到公开证据**：针对 Creo 插件（protk.dat/creotk.dat）自动注册的第三方商业/开源安装器成品（含一键写注册文件 + 环境检测）；安装器对比的"官方三方横评"也未找到权威原文，故本节仅列三家各自的官方能力证据。

---

## 2. Creo 二次开发插件的分发生态（GitHub 项目、CREOSON 等）

### 结论速览
- 生态主流是 **"源码/ DLL 手工分发 + 手工注册"**：GitHub 上的 Creo TOOLKIT 项目（C++ 示例类仓库）以源码形式发布，用户自建并写 protk.dat 注册；未见带安装器的成熟分发形态。
- **CREOSON 是最完整的"产品化分发"先例**：GitHub Releases 下载 + 独立 GUI 配置器 `CreosonSetup`（选择 Creo 版本）+ 自备 `nitro_proe_remote.bat` 启动脚本；而非 MSI/Inno 安装器 + 自动注册。
- CREOSON 4.0 将内置 **MCP 端点 + 可定制 AI Agent Skills**，支持本地/云端 LLM（明确打"本地模型=主权 AI"卖点），与 creo-hub 的 M4 方向高度同构，是最直接的对标物。

### 证据条目
- CREOSON 官方站：MIT 开源、Creo 3.0 起、17,500+ 下载、190+ JSON 函数、HTTP/JSON 接口本地/远程调用；"CREOSON 4.0 will include an MCP endpoint and customizable domain-specific AI agent skills"，可配本地模型（Ollama 等）驱动 Creo——[creoson.com](https://www.creoson.com/)
- CREOSON 安装形态（社区文档）："Download last release of Creoson Server for your system → Run CreosonSetup and configure it with your Creo's version"；要随 Creo 启动需自建 `nitro_proe_remote.bat`（复制 Creo 的 parametric.bat 改名）——[creopyson 文档：Usage/Quickstart](https://creopyson.readthedocs.io/en/latest/_sources/usage.rst.txt)
- CREOSON 发布渠道为 GitHub Releases（下载入口指向 releases 页）——[github.com/SimplifiedLogic/creoson](https://github.com/SimplifiedLogic/creoson)
- GitHub Creo TOOLKIT 项目示例（C++ 源码仓库、无安装器）：L 型拉伸特征示例（Creo Toolkit C API）、点云曲面重建（Creo Toolkit）——[agalex1974/ProToolkitLExtrusion](https://github.com/agalex1974/ProToolkitLExtrusion)、[agalex1974/CreoSurfaceReconstructionFromPC](https://github.com/agalex1974/CreoSurfaceReconstructionFromPC)
- PTC 官方流程同样是"构建→注册→Auxiliary Applications 启动"（官方 makefile 安装示例 `protk_install_example`/`make_install`；GUI 注册见 Auxiliary Applications）——[Creo TOOLKIT Getting Started（PDF）](https://www.ptc.com/support/-/media/support/refdocs/Creo_Parametric/12/Creo_Toolkit_GSG_Creo12400.pdf?sc_lang=ja-JP)、[Creo+ TOOLKIT User's Guide（PDF，Auxiliary Applications 启动）](https://www.ptc.com/support/-/media/support/refdocs/Creo_Plus/latest/plus_tkuse.pdf?sc_lang=en)
- **未找到公开证据**：J-Link / VB API 插件在 GitHub 上以"安装器分发"的先例；也未找到 PTC 官方插件市场/商店（类似 KiCad PCM / Blender Extensions）的存在。

---

## 3. MCP 长任务/进度规范现状（progress、tasks 扩展、elicitation/sampling）

### 结论速览
- **Progress 通知（progressToken）是基础协议的可选机制**，绑定在途请求（请求方在 `_meta.progressToken` 声明，接收方回 `notifications/progress {progress,total,message}`）；适合短任务增量提示，**不解决"任务比连接活得久"**。
- **Tasks 扩展（SEP-2663）已 Final**，提供持久任务句柄：`CreateTaskResult(taskId, TTL, 建议轮询间隔)` → `tasks/get` 轮询 → `tasks/cancel`/`tasks/update`，状态机 `working / input_required / completed / failed / cancelled`；明确针对"外部 job 系统"（把 job id 包装成 task）与断连恢复场景。**轮询是默认，通知（notifications/tasks）可选**。
- **成熟度：规范 Final，运行时生态尚早**——第三方 fastmcp-tasks（2026-10 发布 4.1.0）自述"SEP-2663 是 Final，但生态中只有 schema 和散文规范、无语言 SDK 提供可用运行时"，且"tasks 扩展仍是 experimental，字段级细节可能变动"。
- **Elicitation 已进核心规范**（form 模式 + URL 模式），是"任务中途要用户输入/确认"的标准通道；**Sampling 正被弃用**（SEP-2577 Deprecate Roots, Sampling, and Logging 状态 Final）——LLM 接入不应押注 sampling。
- 官方 Extension Support Matrix 只跟踪 Apps/Auth/Skills 等扩展，**未列 Tasks**；宿主对 Tasks/elicitation 的支持度**未找到官方矩阵级证据**。

### 证据条目
- Progress 规范（2025-11-25）：progressToken 必须为 string/integer 且在活动请求间唯一；接收方 **MAY** 发送 `notifications/progress`（progress、可选 total、可选 message）——[MCP Spec：Progress](https://modelcontextprotocol.io/specification/2025-11-25/basic/utilities/progress)
- Tasks 扩展概览：为何不阻塞（连接/超时问题、崩溃可恢复、进度可见、中途输入、服务端决定是否 task 化）；任务生命周期五状态；"External job systems……return a task when you create the job and resolve it when the job completes"；"Polling is the default. If a server supports notifications, clients can rely on them instead of polling."——[MCP Extensions：Tasks](https://modelcontextprotocol.org/extensions/tasks/overview)
- SEP-2663（Tasks Extension）规范文本，状态 **Final**（含能力协商、CreateTaskResult、tasks/get、tasks/update、tasks/cancel 等章节）——[SEP-2663: Tasks Extension](https://modelcontextprotocol.org/seps/2663-tasks-extension)
- 运行时成熟度证据：fastmcp-tasks 自述是"首个完整服务端实现"、"tasks extension is an experimental MCP extension … the protocol's shape is settled — SEP-2663 is Final — but field-level details may still move"；模型为"立即返回任务句柄 + 客户端按自己的节奏轮询"，工具以 `task=True` 声明意图、由服务端逐次决定——[PyPI：fastmcp-tasks](https://pypi.org/project/fastmcp-tasks/)
- Elicitation 规范（2025-11-25 与 2026-07-28 均为核心 client feature）：form 模式（JSON Schema 结构化收集）+ URL 模式（敏感交互跳出 MCP 客户端）；服务端 MUST NOT 用 form 模式收集密码/API key，客户端 MUST 明示哪个 server 在请求、必须提供拒绝/取消——[MCP Spec：Elicitation](https://modelcontextprotocol.io/specification/2025-11-25/client/elicitation)、[2026-07-28 版](https://modelcontextprotocol.io/specification/2026-07-28/client/elicitation)
- Sampling/Roots/Logging 弃用提案（SEP-2577，列表归于 Final）——[SEP-2577: Deprecate Roots, Sampling, and Logging](https://modelcontextprotocol.org/seps/2577-deprecate-roots-sampling-and-logging)
- 官方扩展支持矩阵仅含 MCP Apps、OAuth Client Credentials、Enterprise Auth、Skills over MCP（无 Tasks/elicitation 行）——[MCP Extension Support Matrix](https://modelcontextprotocol.io/extensions/client-matrix)
- 业界"异步任务状态语义"旁证（非 MCP）：Zoo Text-to-CAD API 任务状态 `queued / uploaded / in_progress / completed / failed` + 人类反馈 `accepted / rejected`——[zoo.dev API：Get a text-to-CAD response](https://zoo.dev/docs/developer-tools/api/ml/get-a-text-to-cad-response)
- **未找到公开证据**：主流 MCP 宿主（Claude Desktop / Cursor / VS Code Copilot 等）对 `io.modelcontextprotocol/tasks` 的实际支持清单；也未找到"MCP server 用 job-id + 独立轮询工具"的官方推荐写法（规范推荐用 Tasks 承载 job id）。

---

## 4. 工程软件内嵌 AI 助手/对话面板先例

### 结论速览
- **交互形态基本收敛为"宿主内对话面板（侧栏/内嵌）+ 工具/命令调用或几何生成"**：Autodesk Fusion 的 Autodesk Assistant、Onshape AI Advisor、SolidWorks 的 Aura/虚拟伙伴均为对话式界面。
- **两种哲学并存**：①**生成式建模**（Autodesk Assistant 直接生成 3D 特征模型、SolidWorks Leo 草图→3D 参数模型、Zoo Text-to-CAD 生成 KCL 代码/几何）；②**问答/建议 + 确定性操作**（Onshape AI Advisor 现阶段=设计环境内的指导、排障、最佳实践建议；CREOSON 路线=190 个确定性 JSON 命令 + AI 只做调用编排）。
- **可靠性论据支持 creo-hub 的"确定性命令"哲学**：第三方实测显示生成式建模的首稿需要人工核验与最终调参（Fusion 生成的瓶子容积不准、记者手动改参数）。
- **离线/内网可行性普遍弱**：Onshape AI Advisor 基于 Amazon Bedrock、SolidWorks AI 跑在 Dassault Outscale "主权云"、Fusion 助手未见离线证据；**本地模型路线目前只看到 CREOSON 明确宣传（Gemma 等本地模型 + MCP）**。

### 证据条目
- Autodesk Fusion「Autodesk Assistant」官方 FAQ / Fusion 帮助页存在（概述与常见问题、支持产品与用法）——[Autodesk：Autodesk Assistant in Fusion – FAQ](https://www.autodesk.com/support/technical/article/caas/sfdcarticles/sfdcarticles/Autodesk-Assistant-in-Fusion-FAQ.html)、[Fusion Help：About Autodesk Assistant in Fusion](https://help.autodesk.com/view/fusion360/ENU/?guid=LEARNINGPANEL)（注：正文抓取被 Autodesk CDN 拒绝，仅确认页面存在与主题）
- Autodesk Assistant 第三方实测（MONOist 2026-06）：一句 prompt 在 Fusion 内生成 PET 瓶 3D 模型（时间线里生成可编辑特征、给出高 200mm/径 65mm/口径 28mm/厚 0.3mm 等参数）；按"容积精确 500mL"追加指令后 AI 用移动面命令改但不准，记者手动调参数；结论是"AI 生成的初稿必须由人核验、最终调参"；并提到社区用 Fusion×Claude（MCP agent）等替代路径——[MONOist：生成AI×3D CADでどこまでできるか試してみた（1/2）](https://monoist.itmedia.co.jp/mn/articles/2606/16/news023.html)
- Onshape AI Advisor（PTC 2025-11-24 新闻稿）：集成进设计环境的对话式界面，提供实时指导、排障工具、最佳实践建议（用户证言强调"对话式界面提问→基于官方内容的可信回答"）；基于 Amazon Bedrock；路线图=agentic workflow（与模型元数据交互、生成 FeatureScript 代码、自动化重复操作）与 AI 辅助渲染——[PTC 新闻稿（意大利文版，内容为官方通稿）](https://www.ptc.com/it/news/2025/ptc-announces-latest-onshape-ai-advisor-release)、[Onshape AI Advisor 产品页](https://www.onshape.com/en/features/ai-advisor)
- SolidWorks / 3DEXPERIENCE AI（DEVELOP3D 2026-02 报道 3DExperience World）：Aura（2025 年推出的 copilot 助手）+ Marie（材料/化学）+ Leo（工程/力学/仿真/制造）三"虚拟伙伴"；基于 Mistral AI 模型、跑在 Dassault Outscale 云（强调保密与安全标准）；Leo 演示=草图→3D 参数化模型（Sketch-to-3D）+ 代理建模做 FEA——[DEVELOP3D：New Solidworks AI agents added at 3DExperience World](https://develop3d.com/cad/new-solidworks-ai-agents-added-at-3dexperience-world/)
- Zoo Design Studio / KittyCAD：Text-to-CAD API 返回 **KCL 代码**（`code` 字段，model 可选 cad/kcl/kcl_iteration），输出 STEP/STL/GLTF 等；任务异步状态 + 人类反馈（accepted/rejected）；另有 Agent API、MCP 文档入口——[zoo.dev API：Get a text-to-CAD response](https://zoo.dev/docs/developer-tools/api/ml/get-a-text-to-cad-response)、[KittyCAD/modeling-app（Zoo Design Studio 应用仓库）](https://github.com/KittyCAD/modeling-app)
- CREOSON 的 AI 路线（对标证据）："Your AI Agents" 走 HTTP/HTTPS MCP 接口 + 可定制 domain-specific agent skills，本地或云端 LLM 皆可，MCP 端点可用 per-system/user access key + TLS 加固——[creoson.com](https://www.creoson.com/)
- **未找到公开证据**：Autodesk Assistant 的离线/内网部署方案；SolidWorks 传统桌面版（非 3DEXPERIENCE Connected）的正式 AI 助手产品文档；各产品"AI 是否允许直接改模型"的官方安全论述原文。

---

## 5. 插件 SDK 模块化与开发者体验（VS Code / Blender / KiCad PCM / FreeCAD）

### 结论速览
- **声明式清单 + 版本化 schema + 宿主兼容区间**是四家共同点：KiCad `metadata.json`（JSON Schema v1/v2）、Blender `blender_manifest.toml`（schema_version）、FreeCAD `package.xml`（format 版本 + XML namespace）、VS Code `package.json` contributes（引擎版本约束）。
- **稳定面/实验面分离**有成熟先例：VS Code 的 proposed API（仅 Insiders、`enabledApiProposals`、`@vscode/dts` 下载 d.ts、VSIX 侧载 + `--enable-proposed-api` 用于内部分发）。
- **脚手架 + 宿主测试桩**是 DX 标配：VS Code Yeoman 生成器自带集成测试，`@vscode/test-cli`/`@vscode/test-electron` 自动下载真实 VS Code 跑 Mocha；cookiecutter 类 C/C++/CMake 模板可作"两文件拷入式 C SDK"的模板蓝本。
- **兼容性向下演进**有成熟做法：KiCad 用 Accept 头双发 v1/v2 仓库文件并做 down-conversion；FreeCAD/Blender/KiCad 都用"宿主版本区间"（min/max）在安装期拦截不兼容包。
- **离线/内网仓库有官方先例**：KiCad PCM 明确支持官方仓库之外的**私有/第三方仓库**（如企业内网仓库），FreeCAD 支持自定义 addon 目录（AddonCatalog.json）。

### 证据条目
- VS Code proposed API（不稳定 API 生命周期）："subject to change、only available in Insiders、should not be used in published extensions"，经迭代后进入稳定 API；`@vscode/dts` CLI 下载 d.ts；内部分发可用 VSIX + `--enable-proposed-api=<extension-id>`——[VS Code 文档：Using Proposed API](https://code.visualstudio.com/api/advanced-topics/using-proposed-api)
- VS Code 测试体系：Yeoman 生成器脚手架自带集成测试；`@vscode/test-cli` + `@vscode/test-electron` 自动下载/解压 VS Code 并在 Extension Development Host 内跑 Mocha，可 `--disable-extensions`、可指定 VS Code 版本与 workspace——[VS Code 文档：Testing Extensions](https://code.visualstudio.com/api/working-with-extensions/testing-extension)
- Blender 扩展清单 `blender_manifest.toml` 字段（官方手册源）：必填 `blender_version_min / id / license（SPDX）/ maintainer / name / schema_version（1.0.0）/ tagline / type（add-on|theme）/ version（semver）`；可选 `blender_version_max / website / copyright / permissions（["files","network"]）/ tags`——[blender-manual 仓库（manual/extensions/index.rst 变更）](https://projects.blender.org/blender/blender-manual/commit/6fc62c5ff98c638c4b8a5192bd85b7c81d35d79e.diff)
- Blender 扩展平台（extensions.blender.org）：仅接受 GPL 兼容开源扩展；发布=上传 + **人工审核队列（Approval Queue）**；Blender 4.2+ 的 Preferences→Get Extensions 安装（首次需允许联网），也可网页拖放安装；更新需手动检查——[extensions.blender.org/about](https://extensions.blender.org/about/)
- KiCad PCM 打包规范：ZIP（ISO 21320-1）+ `metadata.json`；metadata 走 JSON Schema（v1=KiCad 6.0+，v2=KiCad 10.0+，新包应面向 v2）；字段含 reverse-DNS `identifier`、`license`、`versions[]`（version/status/kicad_version/kicad_version_max/download_sha256/download_url/download_size/install_size/runtime）；**schema 版本化**：仓库按 Accept 头（`application/vnd.kicad.pcm.v2+json`）双发 v1/v2 文件并对 v2-only 类型做 down-conversion；官方仓库要求开源、可托管**私有/第三方仓库（如企业内网仓库）**——[KiCad Developer Docs：KiCad Addons（Publishing）](https://dev-docs.kicad.org/en/addons/index.html)
- FreeCAD 插件元数据 `package.xml`（0.20 起）：format="1" + XML namespace；必填 name/version/date/description/maintainer/license/icon/content；version 用 semver 或 CalVer；兼容性声明 `<freecadmin>/<freecadmax>/<pythonmin>`；依赖 `<depend>` 带 version_lt/lte/eq/gte/gt 区间与 type=addon|internal|python，另有 conflict/replace；content 支持 workbench/macro/preferencepack/bundle；workbench 用 `<classname>` 指定 Python 入口类；目录侧有 FreeCAD-addons 的 AddonCatalog.json（按 git 分支分发多版本）——[FreeCAD 文档：Package Metadata](https://wiki.freecad.org/Package_Metadata)
- 脚手架先例（cookiecutter 式 C++/CMake 模板）：moonyl/cmake-cpp-template（"CMake C++ library template project with Cookiecutter configuration, including source code, examples, and test directories"）——[github.com/moonyl/cmake-cpp-template](https://github.com/moonyl/cmake-cpp-template)
- **未找到公开证据**：Blender 官方扩展 CLI（`blender --command extension`）子命令清单原文未能抓取（文档页导航过长被截断），故本节未引用其打包/校验命令细节。

---

## 对 M4 的可迁移启示（可迁移启示清单）

1. 安装器首选 Inno Setup：`PrivilegesRequired=lowest` + `/VERYSILENT /SUPPRESSMSGBOXES /CURRENTUSER`，一套包同时覆盖"无管理员权限一键安装"和"内网无人值守批量部署"。
2. 安装器的"Creo 自动注册"只需做两件幂等动作——写 `protk.dat`/`creotk.dat`、向 config.pro 追加 `toolkit_registry_file`（或 `protkdat`/`creotkdat`）指向它——这是 PTC 官方机制，不需要任何私有 API。
3. 复用 Inno 的 `/SAVEINF=/LOADINF=` 参数预设与 `/LOG` 落盘日志，让企业 IT 能把"Creo 版本/安装路径/是否注册"固化成 INF 文件批量跑。
4. MSI/WiX 仅在客户强制要求 GPO/SCCM 分发时作为第二交付物；其 launch condition 思路（环境检测阻断安装）值得抄进 Inno 的 `[Code]` 检测逻辑。
5. 长任务一律"任务句柄 + 轮询 + 可选推送"建模（对齐 MCP Tasks 的 CreateTaskResult/tasks/get 形态与 working/input_required/completed/failed/cancelled 状态机），不要用阻塞长连接承载 Creo 主线程任务。
6. progress 通知只用于在途请求的轻量进度（progressToken 语义）；Creo 主线程不可抢占的现实决定：真正长的任务必须异步任务化，进度由任务状态/消息字段携带。
7. "任务中途要用户确认/补参"对齐 MCP elicitation（form 模式）与 tasks 的 input_required + tasks/update，避免自造一套确认协议。
8. 面板对话 tab 采用"对话 → 确定性命令调用（可预览/可确认）→ 结构化结果回显"，与 Onshape AI Advisor 的"建议式"现状和 Zoo 的 accepted/rejected 人工反馈语义一致，可靠性论据（生成式首稿需人工核验）由第三方实测支撑。
9. 本地/内网 Agent 是差异化卖点：Onshape（Bedrock）、SolidWorks（Outscale 主权云）、Fusion 均无真正离线方案，CREOSON 4.0 的"本地模型 + MCP + agent skills"是唯一直接对标，creo-hub 应明确打出"全离线/内网 LLM"牌。
10. 插件清单（manifest）用单一声明式文件承载契约：标识（reverse-DNS id）、版本（semver）、宿主兼容区间（creo 版本 min/max，仿 kicad_version/blender_version/freecadmin-max）、入口/文本目录/权限声明（仿 Blender permissions），并配套 JSON Schema 做 CI 校验。
11. schema 自身要版本化并保向下兼容（KiCad v1/v2 + Accept 头双发 + down-conversion 是最佳样板），为"老插件跑在新 hub"留出路。
12. 稳定面/实验面分离照抄 VS Code proposed API 模式：实验 API 打标记、只在 dev 版暴露、写明"勿用于生产插件"，毕业进稳定 SDK 才承诺兼容。
13. Node 脚手架负责"从模板到可注册"的全链路：生成 C SDK 两文件工程 + protk.dat + manifest + 示例命令（cookiecutter/yo code 先例），把注册文件做成模板产物而非手工活。
14. 测试桩按"@vscode/test-electron"思路做：脚手架自带集成测试，CLI 可拉起真实 Creo 会话（或 mock Creo Hub 的 stub）跑插件冒烟测试，插件作者无需真实建模环境也能验契约。
15. 离线部署包内置"本地插件仓库"（KiCad 私有仓库 / FreeCAD AddonCatalog 先例）：一份离线包=hub + 插件目录 + 清单索引 + 校验和（download_sha256 先例），支持 air-gap 环境按索引安装。
16. LLM 辅助接入（skill）走"skill 文件 + 确定性工具"路线，不要押注 MCP sampling（SEP-2577 已弃用 Sampling/Roots/Logging）。
