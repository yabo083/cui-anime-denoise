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

## L-010 移交所有权后立刻清零，就别再读它；测试走的捷径会藏住这条路

**现象**：正式版里画笔/橡皮/套索完全按不动（点了画笔图标、按 B/E/L 都无反应），
但基准 `--protect-e2e` 与全部 12 项交互断言一直是绿的。
**根因**：`job_add_decoded()` 先把 `r->full` 赋给 `j->orig`，再把 `r->full` 清零
（防 double-free），随后 `mask_init(&j->m, r->full.w, r->full.h)` 用了 0×0 尺寸，
calloc 从未发生 → `j->m.px == NULL` → `protect_enter()` 静默 return。
**测试为何漏掉**：`app_test_add_image` / 粘贴路径走**同步** `job_add()`，
那里用的是未清零的 `px.w/px.h` —— 测试覆盖的恰恰不是用户走的那条。
**修法**：读 `j->orig.w/h`（已移交的副本）；e2e 改用 `app_test_enqueue`（真实
loader 路径）并断言 `j->m.px != NULL`。
**教训**：① 所有权转移 + 清零之后，再读原变量是必炸模式——把“用谁”和“清谁”
写在一起才看得见；② e2e 必须走**用户实际走的那条路**，同步捷径只能当单测。

## L-011 事件驱动帧调度下，`now_seconds()` 不是动画时钟

**现象**：处理中环、托盘状态弧、启动 banner 的弧都肉眼卡顿/不动。
**根因**：CUI 只在 `cui_needs_redraw()`（animating/settle）时连续给帧；
手绘弧读 `now_seconds()` 画，不置 animating → 过渡结束后帧停了，
弧只在有输入事件时挪一格。
**修法**：`fl_spinner()`（仿 `cui__draw_spinner` 形状：暗轨道 + 呼吸头弧）
统一替代手绘弧，并 `cui_request_redraw()`；其它 now_seconds 动画
（蒙版淡出、结果交叉淡入、重跑扫条、已保存✓）按需 request_redraw /
request_wakeup。`ui->time`（`cui_time()`）才是动画时钟。

## L-012 无边框窗口的边缘光标，要同时赢两处

**现象**：拖到窗口边缘时缩放光标很难出现/闪跳。
**根因**（双份）：① chrome 的 WM_SETCURSOR 对 HTCLIENT 也抢着答 arrow，
把 CUI 组件悬停光标（hand/ibeam）每次移动都踩掉；② CUI 每帧
`cui_end_frame → set_cursor` 只看"指针在客户区"——无边框窗口客户区=
整窗，覆盖到我们自制的 resize band，动画期间每帧把缩放光标踩回箭头。
**修法**：① WM_SETCURSOR 只认领非客户区命中码（resize 带 + caption），
HTCLIENT 放行给 CUI 自己的 handler；② 给 vendored `cui_app_win32.c`
的 `cui__win32_set_cursor` 加 `WM_NCHITTEST == HTCLIENT` 门槛——
非客户区不应用 widget 光标（对普通 CUI 窗口行为不变）。

## L-013 手写宽度公式与 x-walk 是两份真相，必然漂移

**现象**：保护工具条右边被切掉约 14px（「完成」露一半），保存钮的「已保存」
tick 也溢出 8px。
**根因**：容器宽度和内容布局是两段独立代码——公式漏了几个 `UI_S4`，后来
tick 加宽按钮时又没同步改。维护任何一边都不会提醒另一边。
**修法**：`protect_lay(paint)` 一次 x-walk 同时负责测量与绘制，
`paint=false` 先量出自然宽再 `paint=true` 画；宽度永远等于内容。
tick 也不再改按钮宽（同字数文案，省掉宽度脉冲）。
**教训**：即时模式 UI 里"尺寸=布局之和"的地方，宁可多走一遍 layout
也别复制一份求和式。

## L-014 模态缩放下，没有 frame_cb 的窗口只能重播旧帧

**现象**：拖窗口边缘放大时，右边和底边露出大片空白，UI 缩在旧尺寸里。
**根因**：主循环是手写 `while (cui_app_frame())`，`app->frame_cb` 没人填；
OS 缩放循环里 `cui__app_live_frame` 走 `!frame_cb` 分支，只把**旧尺寸**的
draw data 再贴一次。
**修法**：给 vendored CUI 加 `cui_app_set_frame`（`cui_app_run` 也走它），
main 注册回调后模态缩放期间每帧都按新尺寸重建。
**教训**：手动接管事件循环时，框架在 OS 模态循环（缩放、菜单、移动）
里的"备用绘制路径"需要显式接线，否则静默退化。

## L-015 行为平价要看数据结构，不是看画出来的样子

**现象**：套索"过程中贴合"，点与点之间却是直线——和原项目手感不同。
**根因**：`st->lasso` 存的是稀疏锚点，吸附的 live wire 只活在预览里；
web 版 `L.pts` 是**密集折线**（每个吸附点都进数组），`L.anchors` 只记
每段终点下标。数据模型不同，画出来的逼近度就不同。
**修法**：改成同一模型——commit 时把整条 wire（跳过共享首点）追加进
lasso，anchor 记段尾下标；Backspace 按下标截断；闭合时再算一条回起点的
wire；填充/预览都用密集折线。
**教训**：移植交互功能先读原实现的状态结构；"看起来差不多"的简化通常
省掉的就是用户能感到的那部分。
