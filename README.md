# Anime Denoise

原生 Windows 桌面版 GPT 二次元图去噪工具。界面用 [CUI](https://github.com/yappologistic/CUI)（即时模式 UI，C）绘制，
推理由进程内的原生引擎完成（ONNX Runtime + DirectML/CPU，`src/engine/` + `src/denoiser.c`），
去噪算法等价于 [gpt-anime-denoise](https://github.com/CXP-2024/gpt-anime-denoise)（MIT）的 `denoise.py`。

所有处理都在本机完成，不上传任何图片。

## 功能

- **去噪**：三档预设（轻柔 80% / 推荐 60% / 强力 45%）+ 预缩比例与 L0 平滑两个滑块；改参数即重跑当前图。
- **局部保护**：`B` 进入保护模式——笔刷 / 橡皮 / 磁力套索，柔边自动过渡，撤销重做（Ctrl+Z / Ctrl+Shift+Z），
  `I` 反选。涂过的地方恢复原图，其余保持去噪结果；成品另存为 `…_protected.png`。
- **对比视图**：左右可拖动分隔线，`1/2/3` 切换对比 / 原图 / 结果，`X` 在原图↔结果互切，
  按住 `Z` 临时看原图，按住 `M` 看保护区域。
- **画布底**：默认毛玻璃——取最近一次输出重度模糊后盖主题色纱（类 Mica 动态取色）；
  设置里可切回纯色棋盘格。
- **多图**：拖入、`Ctrl+O` 选择、`Ctrl+V` 粘贴，底部胶片托盘 + 左右方向键切换，托盘瓦片悬停出删除按钮。
- **缩放**：滚轮以外的 `Ctrl+=` / `Ctrl+-`、`F` 适合窗口、`0` 1:1。
- **输出**：自动写入输出目录，`Ctrl+S` 另存为；透明通道与 ICC 色彩配置保留。
- **设备**：自动 / GPU（DirectML）/ CPU，设置里可切换，失败自动回退 CPU。
- 浅色 / 深色 / 跟随系统主题；中文界面，HarmonyOS Sans 字体，Lucide 图标。
- 自更新（GitHub Release 资产 `update.json` + SHA-256 校验 + Inno Setup 静默升级）、
  日志轮转、崩溃报告、诊断包导出。

## 安装

`dist/AnimeDenoise-<版本>-setup.exe` 是每用户安装（无需管理员），默认装到
`%LOCALAPPDATA%\Programs\AnimeDenoise`。安装向导里两个可选集成默认开启：

- **加入用户 PATH**：装完任意目录都能直接敲 `anime-denoise`。
- **图片右键菜单**：`用 Anime Denoise 去噪` 以该图启动 GUI（所有系统已标记为
  image 的类型 + avif / heic / heif）。

安装包自带全部推理资产：`runtime/`（onnxruntime.dll + providers_shared + DirectML.dll）、
`models/`（随包模型）、`assets/`（字体、图标、许可证）。**无需 Python、无需首次下载环境**。
卸载只动安装目录；`%LOCALAPPDATA%\AnimeDenoise` 里的设置、日志、输出与已下载模型默认保留
（交互式卸载会问一句要不要一起删）。

## 命令行

同目录下的 `anime-denoise.exe` 是独立 CLI（GUI 的全部引擎能力，无界面）：

```bash
anime-denoise photo.png                      # 默认 esrgan60_l0
anime-denoise a.png b.png folder\ --method cugan60_l0
anime-denoise x.png --out out\ --down 45 --l0 0.002 --device gpu
anime-denoise x.png --all                    # 7 种方法 + 对比表 _sheet.png
anime-denoise --download cugan               # 预下载可选模型权重
type x.png | anime-denoise - --stdout > y.png   # 管道模式（stdin/stdout）
anime-denoise in.png --json --quiet          # 机器可读：每完成一项输出一行 JSON
```

退出码：0 全部成功 / 1 参数或输入错 / 2 部分失败 / 3 引擎或环境不可用。
完整契约见 `docs/cli-spec.md`。

## 从源码构建

工具链全部由 [mise](https://mise.jdx.dev) 管理，见 `mise.toml`：

```bash
mise install            # python / node（资产子集脚本用）
mise run build          # Release，产物 build/AnimeDenoise.exe + build/anime-denoise.exe
mise run run            # 构建并启动
mise run dist           # 打安装包到 dist/（需要 Inno Setup 6 的 ISCC.exe）
```

构建需要 Visual Studio 的 C++ x64 工具集与 PowerShell 7；`build.ps1` 通过 `toolchain.ps1` 自己
捕获 vcvarsall 环境，不要求预置 Developer Command Prompt。`build/` 下同时产出 CLI 工具
`anime-denoise.exe`（用法见 `docs/cli-spec.md`）与 GUI 的 `runtime/`、`models/` 副本。

打包前置条件（一次性，产物进 `build/`）：`pwsh tools/fetch_deps.ps1` 下载钉版
ONNX Runtime + DirectML DLL 和 esrgan 模型；`models/*.skel.onnx` 已入库（由
`tools/export_onnx.py` 生成，改模型集时才需要重跑）。细节见 `installer/build_installer.ps1` 头注。

`mise run assets` 从 `_dl/` 里的原始字体重新生成子集（HarmonyOS Sans 的 GB2312 子集、完整 Lucide 图标字体），
`mise run check-fonts` 校验 `src/` 里出现的每个非 ASCII 字符都在 UI 字体里 —— 构建前的守门检查。

## 模型

| 模型 | 来源 | 随包 |
|---|---|---|
| ESRGAN anime-6B ×4 | `models/esrgan_anime6b_x4.onnx`（18 MB） | ✅ 随安装包 |
| Real-CUGAN ×2 | `cugan_up2x_denoise3x.skel.onnx`（31 KB，入库） + `up2x-latest-denoise3x.pth` | 骨架随包，权重按需在设置页/`--download` 下载 |
| APISR RRDB ×2 | `apisr_rrdb_x2.skel.onnx`（108 KB，入库） + `2x_APISR_RRDB_GAN_generator.pth` | 同上 |

骨架是无权重的 ONNX 图，把上游 `.pth` 当 external data 读；下载落到
`%LOCALAPPDATA%\AnimeDenoise\models` 并做 SHA-256 校验。整包模型权重不随安装包分发。

## 目录

```
src/
  main.c        进程入口：窗口、事件循环、命令行
  app.c         状态机：denoiser 事件、意图、帧编排
  app_jobs.c    任务生命周期、加载队列、提交、保护合成调度
  app_input.c   快捷键表、拖放、粘贴、另存、保护模式进出
  ui_shell.c    40px 标题栏、设置侧栏、帮助/关于/更新对话框
  ui_stage.c    画布：对比分屏、遮罩叠加、笔刷光标、缩放
  ui_float.c    浮层：主控 pill、胶片托盘、保护工具条、微调卡、横幅、空态
  ui_lasso.c    磁性套索：锚点、live-wire、闭合/减选
  ui_fluent.c   Fluent 调色板 + 自绘控件原语（分段/按钮/滑杆/图标钮）
  chrome.c      自绘标题栏：命中测试、缩放光标、最大化、控件区 zones
  denoiser.c    推理工作线程：任务/事件环、SR 缓存、设备与模型管理
  loader.c      解码线程：解码 + 缩放 + 缩略图
  img.c         WIC 解码/编码/缩放/剪贴板/保护合成
  mask.c        蒙版：笔刷、套索、羽化、瓦片撤销、磁力套索
  config.c      设置与路径
  log.c         日志轮转 / 崩溃报告 / 诊断包
  update.c      更新状态机
  update_io.c   WinHTTP / JSON / SHA-256
  ui_tokens.h   视觉 token 单源
  engine/       原生推理引擎：ONNX Runtime + DirectML 装载、
                Pillow 逐字节 resample、pocketfft L0、模型下载；
                对外契约 engine.h，规范 design.md §7
  cli/          独立命令行 anime-denoise.exe（共享 anime_engine）
models/          随包 ONNX 模型
assets/          字体子集、完整 Lucide、许可文件
installer/       Inno Setup 脚本与打包脚本
docs/            design.md / fluent-redesign.md（布局权威）/ cli-spec.md
.context/        ADR / 当前状态 / 踩坑
tests/           test-suite.md（行为规格）+ 各模块独立测试
```

## 许可

- 本程序：仓库内 LICENSE。
- 去噪算法参考 `denoise.py`：MIT，© CXP-2024。
- 字体：HarmonyOS Sans（华为设备有限公司授权，见 `assets/fonts/LICENSE-HarmonyOS-Sans.txt`）。
- 图标：Lucide，ISC。
- UI 框架：CUI，MIT。
- 第三方许可全文随安装包分发在 `assets/` 下。

## 线程

- UI 线程只做布局、贴图上传与状态迁移；引擎启动是异步的（`dn_start` → `DN_EV_READY`），
  窗口 0.8 s 内可用，启动期间任务停在「排队中」。
- 一个 below-normal 的 denoiser 线程独占 `eng *`：推理、SR 缓存、设备切换、模型下载全部串行在其上，
  UI 只投递任务、每帧收事件。
- 图片解码在 `loader.c` 的线程里完成，UI 每帧只取结果并建纹理（实测 0.37 ms/张）。
- 只有原生模态对话框会阻塞 UI。

## 性能与交互基准

性能回归由 `lab/测试工程` 覆盖：13 个场景 × 1000 次迭代，外加 13 项交互断言；
`--shots <dir>` 用软件光栅器对真实应用帧截图，供 Fluent 布局走查。
产物是 `lab/测试工程/report/latest/report.md`（含环境、方法、逐场景 p50/p95/p99 与判定）。
它抓到并推动修掉了一个真实问题：笔刷涂抹时每帧重算整张蒙版羽化，帧成本 5.8 ms，
改为 40 ms 节流后降到 0.04 ms。
