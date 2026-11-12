# 架构决策记录（ADR）

## ADR-001 用 CUI 即时模式而非 Web/Electron

**背景**：原工具是 Python + 浏览器 GUI，启动慢、体积大（Electron ~150 MB）。
**决定**：C + CUI 自绘窗口，单 exe 512 KB。
**代价**：控件要自己拼；无 CSS 生态，一致性必须靠 token 与规范（见 design.md §4）。
**状态**：已采纳。

## ADR-002 推理放在独立 Python 进程

**背景**：torch 无法嵌入 C 进程；模型更新频繁。
**决定**：`worker.py` 以 JSONL over stdio 与宿主通信，宿主只管调度。
**代价**：进程间序列化、跨进程错误处理；必须处理 Windows 上 stdin-reader 与 .pyd 导入的死锁（见 LESSONS）。
**状态**：已被 ADR-008 取代（torch 非必需被 Phase 1/2 证伪，worker.py 已删除）。

## ADR-003 蒙版合成放在 worker

**背景**：全分辨率去噪结果只在 worker 侧；传到宿主再合成要 67 MB/张。
**决定**：宿主上传 8-bit 灰度蒙版（PNG+base64），worker 用缓存的去噪结果合成并写盘。
**代价**：合成要走一次进程往返（≈0.5 s @2096²）；需按 worker job id 命中结果缓存。
**状态**：已被 ADR-008 取代 —— 进程内推理后结果像素本来就在宿主手里，
`img_composite_masked` 直接合成，无 IPC。

## ADR-004 运行时按需安装，不随包分发

**背景**：CUDA 版 torch 约 2–3 GB，装进安装包会让分发体积失控。
**决定**：安装包 4.6 MB；首次运行用 uv 装配 Python + torch + 依赖到 `%LOCALAPPDATA%`。
**代价**：首启需要网络与 2 GB 下载；必须给出可视进度与可重试的失败态。
**状态**：已被 ADR-008 取代 —— 原生运行时只有 ORT+DirectML ≈35 MB，
随包分发比按需安装更简单；`runtime.c` 已删除，`%LOCALAPPDATA%\models`
只存按需下载的 CUGAN/APISR。

## ADR-005 基准用无窗口 CUI 上下文

**背景**：UI 性能回归需要可复现、无人值守的数字。
**决定**：`lab/测试工程` 链接 `anime_core`，用 `cui_create` + 桩渲染器跑真实 `app_frame`。
**代价**：不含 GPU 上传与合成；报告里明确标注“UI 侧成本”。
**状态**：已采纳。

## ADR-006 自绘标题栏，保留 WS_THICKFRAME

**背景**：原生标题栏与自绘顶栏重复。
**决定**：`WM_NCCALCSIZE` 清零客户区外的所有区域，`WM_NCHITTEST` 自行划分拖动/缩放/按钮区，保留 `WS_THICKFRAME` 以维持阴影、吸附与最大化动画。
**代价**：必须自己处理 DPI、最大化 inset 与 `WM_SETCURSOR`，否则缩放“看起来坏了”。
**状态**：已采纳，命中测试与光标部分重做中。

## ADR-007 原生 C 推理引擎取代 Python 侧车推理

**背景**：Python worker 的推理路径要求 3.1 GB 运行时（uv + Python + torch），首启下载约 2 GB；
实际算法只有三个固定 ONNX 图（Real-ESRGAN / CUGAN / APISR）加一个 L0 FFT 平滑，torch 不是必需的。
**决定**：新增 `src/engine/`：动态装载 onnxruntime.dll，DirectML EP（高性能适配器，失败回退 CPU EP）；
resize 逐字节移植 Pillow 8bpc；L0 用 pocketfft 逐位复刻 float64 参考实现；
模型走 ONNX —— ESRGAN 权重内嵌随包，CUGAN/APISR 用无权重骨架把上游 .pth 当 external data 读。
**代价**：瓦片、设备回退、sanity tile 都要自己维护；ORT+DirectML 运行时约 35 MB 需随包或按需下载；
APISR/CUGAN 的 .pth 仍由用户下载（5–18 MB，不再是 2 GB）。
**状态**：已采纳。Phase 2 引擎层完成并通过逐位平价（SR ±1 级 ≥99.98% 一致、同输入 L0 逐字节一致）；
UI 接入由 ADR-008 收口。

## ADR-008 进程内 denoiser 线程取代 Python 侧车（Phase 4a）

**背景**：引擎（ADR-007）就绪后，Python worker 只剩一个壳——把宿主像素序列化过去、
再把结果传回来。3.1 GB 运行时、首次装配、stdio 协议与进程生命周期全是这一壳的成本。
**决定**：新增 `src/denoiser.c`：一条 `THREAD_PRIORITY_BELOW_NORMAL` 线程独占
`eng *`，任务/事件用有界环（满时只丢 PROGRESS，终结事件不丢）；UI 只投递
`dn_push_denoise`（RGBA 像素直接拷贝进环，无 IPC）并每帧 `dn_poll`；
合成与写盘移回宿主（`img_composite_masked` + `img_encode_png_icc`，保留 alpha/ICC）；
SR 结果缓存 2 项 LRU（键 = src_key+down_pct）使改 L0 重跑不重复推理；
60 s 空闲释放推理会话。worker.py、backend、runtime 装配、协议文档全部删除。
**代价**：合成+羽化+写盘落在 UI 线程（受控：笔画结束触发，4096² ≈40 ms 可接受）；
`app.c` 涨到 1251 行超出 ≤800 上限，待按职责拆（已登记 STATE.md）。
**状态**：已采纳。E2E 验证：启动→READY→去噪→写盘 16.3 s，无 Python 进程。

## ADR-009 画布优先 Fluent UI：侧栏布局废弃，控件全部进画布浮层（Phase 4b）

**背景**：侧栏时代 UI 有 4 个常驻分区（顶栏/侧栏/缩略图条/状态行），但用户操作中心
只有画布；规范 `docs/fluent-redesign.md` 要求 40px 标题栏 + 全幅画布 + 按需浮层。
**决定**：`ui_shell.c` 重写为标题栏画布 + CUI 侧栏/对话框；`ui_stage.c` 只留画布地板、
对比分屏、遮罩叠加、笔刷光标；新增 `ui_float.c`（pill/托盘/保护条/微调卡/横幅/空态）、
`ui_lasso.c`（磁性套索）、`ui_fluent.c`（Fluent 调色板上 token + 自绘原语）。
浮层控件全是画布内自绘：绘制帧把命中矩形写进 `app_state.r_*`，下一帧
`ui_overlay_input` 消费；标题栏矩形另经 `chrome_set_zones` 通报 `WM_NCHITTEST`，
否则控件区被当拖动带。`app.c` 先行三分（state/jobs/input，ADR 前提交 b664fd1）。
**代价**：新控件要遵守「先画矩形、下帧接输入」的两拍协议；命中矩形散落在
`app_state`，控件多时靠命名纪律管理。规范外出入全部登记
`docs/fluent-redesign.md` §10（文件名链接、设备胶囊、Esc 语义、保护快照取消等）。
**状态**：已采纳。13 张软件光栅截图走查通过；bench 12/12 交互断言、
13 场景 p95 全在预算内；真实启动冒烟通过（标题栏/托盘/保护条/设置页/深色）。
