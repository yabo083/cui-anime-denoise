# AGENTS.md — cui-anime-denoise（L0 宪法与路由）

Windows 原生去噪工具：C + CUI 即时模式 UI，进程内 ONNX Runtime（DirectML/CPU）推理，Inno Setup 分发。
**规矩写在 `docs/design.md`（规范）里，本文件只负责“改哪块该读什么”。**

## 铁律

1. UI 线程不得阻塞：引擎启动是异步的（denoiser 线程回 READY 事件），图像解码走加载队列，帧内不碰网络/长等待。
   违反即视为缺陷，见 `docs/design.md` §2。
2. 视觉只用 `src/ui_tokens.h` 的 token；同角色控件必须同一实现路径。见 §4。
3. 单个 `.c` 的行数上限按职责类型分：
   - **交互/状态类**（`app.c`、`ui_*.c`、`denoiser.c`、`chrome.c`）≤ 800 —— 它们的状态耦合最容易变坏；
   - **单职责叶子模块**（`img.c` WIC、`mask.c` 算法、`config.c` 设置/路径、`log.c` 日志/崩溃/诊断包、`update.c` 网络、`engine/*.c` 推理引擎）≤ 1200
     —— 拆开只会把相互调用的紧耦合代码撕成两半，可读性反而下降。
   超限或职责混杂时按职责拆文件，并在下表登记。
4. 改完必须跑：`pwsh -NoProfile -File build.ps1`（/W4 零警告）+ `lab/测试工程` 的基准与交互断言。
5. 临时脚本只进 `tmp/`，交付前删除。

## 路由表（先读文档，再读代码）

| 要改什么 | 先读 | 再读代码 |
|---|---|---|
| 线程模型、denoiser 事件、卡顿 | `docs/design.md` §2/§5 | `src/denoiser.c`, `src/app.c` |
| 任务生命周期、队列、合成调度 | `docs/design.md` §2/§5 | `src/app_jobs.c` |
| 快捷键、拖放、粘贴、另存 | `docs/design.md` §4/§5 | `src/app_input.c` |
| 窗口边框、拖动、缩放、标题栏 | `docs/design.md` §3 | `src/chrome.c`, `src/main.c` |
| 界面外观、组件尺寸、一致性 | `docs/design.md` §4、`src/ui_tokens.h` | `src/ui_shell.c`, `src/ui_stage.c` |
| 画布交互、笔刷、套索、缩放 | `docs/design.md` §4/§5 | `src/ui_stage.c`, `src/mask.c` |
| 保护蒙版算法、羽化、撤销 | `src/mask.h` 注释、`lib` 里 `mask.c` 文件头 | `src/mask.c` |
| 图片编解码、剪贴板、保护合成 | `src/img.h` 注释 | `src/img.c` |
| 设置、路径、日志、崩溃、诊断包 | `src/config.h` 注释 | `src/config.c`（设置/路径）、`src/log.c`（日志/崩溃/诊断包） |
| 更新与安装包 | `src/update.h` 注释、`installer/` | `src/update.c`, `installer/*.iss` |
| 原生推理引擎、设备回退、瓦片、L0、模型下载 | `docs/design.md` §7、`src/engine/engine.h` 注释 | `src/engine/*.c` |
| 行为规格（该出现什么效果） | `tests/test-suite.md` | `lab/测试工程/bench/bench_main.c` |
| 历史决策与踩坑 | `.context/DECISIONS.md`、`.context/LESSONS.md` | — |

## 目录边界

```
src/app.c      状态机、denoiser 事件、意图处理（不含绘制）
src/app_jobs.c 任务生命周期、加载队列、提交、宿主侧保护合成
src/app_input.c 键盘命令、拖放、粘贴、另存为、文件对话框
src/ui_shell.c 顶栏 / 侧栏 / 缩略图条 / 状态行
src/ui_stage.c 画布：对比、笔刷、套索、缩放
src/denoiser.c 进程内推理工作线程：任务/事件环、SR 缓存、设备与模型管理
src/loader.c   图片解码线程（解码 + 缩放 + 缩略图，UI 只建纹理）
src/img.c      WIC 解码/编码/缩放/剪贴板/保护合成
src/mask.c     蒙版：笔刷、套索、羽化、瓦片撤销
src/config.c   设置与路径
src/log.c      日志轮转 / 崩溃报告 / 诊断包
src/update.c   更新状态机与公开 API
src/update_io.c WinHTTP、JSON、SHA-256（update 专用）
src/engine/engine.h    引擎契约：线程约定、设备/模型枚举、错误语义
src/engine/engine.c    ORT 装载：DML/CPU EP 会话、瓦片推理、流水线（叶子，≤1200）
src/engine/resample.c  Pillow 8bpc BOX/LANCZOS 逐字节移植
src/engine/l0.c        L0 平滑（pocketfft，float64，多线程）
src/engine/models.c    APISR/CUGAN .pth 状态与下载（复用 update_io 助手）
models/        随包 ONNX 模型（esrgan_anime6b_x4 + CUGAN/APISR 骨架）
assets/        字体（HarmonyOS Sans 子集 + Lucide）
installer/     Inno Setup 脚本
```

## 提交约定

- 一个切片一个提交，message 写“为什么”，不写“改了什么”。
- 动了 `app.c`/`ui_*.c` 的行为，同时更新 `tests/test-suite.md` 与 `lab/测试工程` 的断言。
- 结论性的踩坑写进 `.context/LESSONS.md`，架构选择写进 `.context/DECISIONS.md`。
