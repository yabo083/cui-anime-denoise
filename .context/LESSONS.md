# 踩坑记录（根因 + 修法，不含流水账）

## L-001 Windows：线程阻塞在 stdin.readline 时另一线程 import .pyd 会死锁

**现象**：侧车收到 `init` 后永久沉默，无 traceback。
**根因**：读线程阻塞在 `sys.stdin.readline()`，主线程同时 import numpy/torch 的扩展模块 → CRT 加载器层面死锁。
**修法**：主线程先同步读第一行 `init`，完成全部 import 与模型加载，之后再启动 reader 线程。
`for line in sys.stdin` 另有 8 KB readahead 吞命令的问题，一律显式 `readline()`。

## L-002 worker 启动阻塞在 UI 线程 → 整个窗口冻住

**现象**：首次启动 4–7 s 白屏，模型缺失时最长可冻 180 s。
**根因**：`wb_start()` 内部 `CreateProcessW` 之后同步等待 `ready` 事件；UI 线程在帧里调用它。
“先画一帧再启动”的延迟只把冻结推迟一帧，没有解决。
**修法**：拆成 `wb_start_async()`（立即返回）+ 事件驱动的 BOOTING/READY/FAILED 状态机；
启动期间任务是 `QUEUED`，UI 全程可交互。
**教训**：**“延迟一帧”不是异步**。判断标准是这条路径上是否还有 `WaitForSingleObject`。

## L-003 CUI 的 `cui_size` 字形初始化：`.width = 60` 能编译但语义错误

**现象**：缩略图不按给定尺寸绘制、位置错乱。
**根因**：`cui_size` 是 `{kind, value}` 结构体；MSVC 对指定初始化里的标量宽容，
`.width = 60` 得到 `kind = 0 (FIT)`，尺寸被当成“内容自适应”。
**修法**：一律 `CUI_PX(60)` / `CUI_FILL`。赋给 `float` 字段的（如 `cui_progress_opts.height`）**不能**包 `CUI_PX`。

## L-004 `cui_dialog` 是 for 语句，不能塞进 if

**现象**：`if (cui_dialog(...)) { ... }` 报 “语法错误: for”。
**根因**：`CUI__SCOPE_IF` 展开成 `for (...)`，本身就是条件语句。
**修法**：直接写 `cui_dialog(...) { ... }`；`cui_row/column/scroll/card` 同理（它们是无条件作用域）。

## L-005 WIC PNG 编码器会静默改写像素格式

**现象**：写出的 PNG 红蓝互换。
**根因**：请求 `GUID_WICPixelFormat32bppRGBA`，`SetPixelFormat` 返回 S_OK 却把格式改成 `32bppBGRA`。
**修法**：设置后回读实际格式，若是 BGRA 则手工交换通道；alpha 保持直通（不预乘）。

## L-006 蒙版浮层每帧重建 → 涂抹时 5.8 ms/帧

**现象**：笔刷涂抹时明显掉帧（基准里 `brush_paint` p50 5.8 ms）。
**根因**：每个鼠标移动都触发整张蒙版羽化（2048² 约 18 ms 的一部分）+ 重建 RGBA。
**修法**：涂抹期间覆盖层按 40 ms 节流重建，笔画结束后再做一次精确重建 → p95 0.042 ms。
**教训**：交互路径上只做增量；“精确”留给停顿时刻。

## L-007 滑块拖动时每帧写 settings.json

**现象**：拖滑杆时磁盘持续写入，且设置文件可能半写。
**根因**：`settings_from_state()` 每次值变化都 `cfg_save()`。
**修法**：内存置脏标记，停止拖动 500 ms 或关窗时落盘。

## L-008 全不透明 RGBA 经 WIC 写 PNG 仍是 32bpp，破坏对 Pillow 的平价

**现象**：CLI/GUI 输出的 PNG 全是 `8-bit/color RGBA`，而 `denoise.py`（Pillow）对无 alpha 的图写 RGB。
**根因**：宿主内部一律用 straight RGBA8；`img_encode_png_icc` 无脑请求 32bpp，
即使 alpha 通道全 255。
**修法**：编码前扫一遍 alpha——全 255 走 24bpp BGR（WIC 自动丢 alpha），
有真 alpha 才走 32bpp。判断代价是 O(n) 一次内存扫描，相对编码可忽略。
**教训**：像素格式平价要在**编码出口**对齐，内部表示统一不代表输出一致。

## L-009 绝对耗时门遇到共享机器：先看 p50 是否双模，再判回归

**现象**：`mask_feather` p95 从 21.6 ms 飙到 68 ms，`img_decode` 40→45 ms，报告判 FAIL；
但两个场景的代码一行没动。
**根因**：不是回归——p50 仍是 18.5/38.7（与历史持平），>5% 的尾部到 ~70 ms，
呈双模分布：周期性外部干扰（远程会话采集工具 PresentMon/GameViewer 虚拟显示在跑）
间歇偷内存带宽。提高进程优先级无效（不是调度抢占）。
**修法**：先把隔离场景跑短迭代看 p50/p95 是否分裂；p50 与历史持平+p95 独高 → 环境噪声，
写进报告如实记录，不要为了消 FAIL 改没问题的代码。等干扰源退出后重跑确认。
