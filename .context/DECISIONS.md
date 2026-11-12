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
**状态**：已采纳。

## ADR-003 蒙版合成放在 worker

**背景**：全分辨率去噪结果只在 worker 侧；传到宿主再合成要 67 MB/张。
**决定**：宿主上传 8-bit 灰度蒙版（PNG+base64），worker 用缓存的去噪结果合成并写盘。
**代价**：合成要走一次进程往返（≈0.5 s @2096²）；需按 worker job id 命中结果缓存。
**状态**：已采纳。

## ADR-004 运行时按需安装，不随包分发

**背景**：CUDA 版 torch 约 2–3 GB，装进安装包会让分发体积失控。
**决定**：安装包 4.6 MB；首次运行用 uv 装配 Python + torch + 依赖到 `%LOCALAPPDATA%`。
**代价**：首启需要网络与 2 GB 下载；必须给出可视进度与可重试的失败态。
**状态**：已采纳，端到端验证过一次（3.1 GB runtime，随后成功去噪）。

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
UI 接入是后续切片。
