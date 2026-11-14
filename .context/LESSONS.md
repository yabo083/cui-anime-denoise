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

## L-016 门控输入层时必须继续喂边沿跟踪器

**现象**（本轮自review发现）：设置抽屉通过齿轮点击打开后，关闭抽屉的
第一次点击被吞——对比分隔线会变成平移、浮层按钮完全没反应。
**根因**：`ov_pressed`/`mouse_pressed`/`tb_pressed` 各自用 static `was`
做按下沿检测。模态门让 `ui_overlay_input`/`stage_handle_input` 整帧不跑，
但**开门那一下按下沿**已经在开门帧把 `was` 置真；模态期间的松开永远
到不了跟踪器 → 门一关，`down && !was` 永假，第一次真实点击失去按下沿。
**修法**：导出一对"喂跟踪器"函数（`ui_overlay_track`/`stage_track_buttons`），
门控分支里只跑跟踪器不跑语义——边沿状态跟真实按键走，输入仍被截断。
**教训**：手工分层的即时模式输入，每个 `was` static 都是一份隐藏的
输入状态机；旁路一层时要想"它漏掉的边沿谁来补"。

## L-017 同一消息有两个消费者时，子类化别独吞

**现象**：设置抽屉内容超出一屏时滚轮不滚——CUI 的 scroll view 全员哑火。
**根因**：`chrome.c` 的子类拦截 `WM_MOUSEWHEEL` 充给画布缩放累加器后
直接 `return 0`，CUI 的 wndproc 从没见过这条消息。
**修法**：累加之后 `CallWindowProcW(g_orig, ...)` 转发给原过程——两个
消费者各取所需；模态门在帧层决定累加量是否当缩放用。
**教训**：窗口子类化的 "consume" 要按语义判断：累加采样 ≠ 消费消息。

## L-018 打开模态的"方式"会影响隐藏的输入状态

I13 最初只测"标志位开栏 → 键鼠不穿透"。真实路径是**点击打开**：
按下沿本身会写进各层的 `was`。bench 里把"按下→开栏→栏内松开→关栏→
再按"排成序列后，才复现出"关栏后首击被吞"（L-016）。
**教训**：测模态/门控类行为时，驱动方式要走真实路径（按键按下沿、
按下保持），只翻标志位测不出跨帧的边沿状态。

## L-019 字体覆盖门扫描的是整个文件，不是字符串字面量

**现象**：帮助对话框 Esc 行渲染出豆腐块——→(U+2192) 不在
HarmonyOS 子集 cmap 里。跑 `gen_assets --check` 才发现，同时报出
的还有注释里的 `≈ ≤ ─ □`（根本不参与渲染）。
**修法**：渲染文本改用字体已有的 `·`；注释退回纯 ASCII。
**教训**：给源码加非 ASCII 字符前先跑 `gen_assets --check`——
门把注释当渲染文本管，写注释也只能用字体/ASCII 覆盖的字。

## L-020 CUI popup 的"存活期"比 open 标志长一拍

**现象**：I14 断言 `Esc 关菜单 → 按 2 恢复` 失败——Esc 已把
`p->open` 置 false，但 `cui__popup_begin` 在退出弹簧（SPRING_EXIT
0.16s ≈ 10 帧）沉降前一直返回 true，`ctx_open` 随之保持，门没撤，
后面的 Ctrl+C 全被门挡住。
**修法**：断言里 `pump(2)` 改 `pump(20)`；`ctx_open` 语义定义成
"surface 还在"（含淡出），门控按这个语义走是对的。
**教训**：`popup_begin` 返回值 ≠ open，是"仍在构建/动画中"。凡拿它
当门控或断言条件的，都要把退出动画算进时间预算。

## L-021 锚定整幅画布的 context menu 会被锚宽撑爆

**现象**：`cui_context_menu` 绑在 stage canvas 上，菜单横向拉满整个窗口——
`cui__popup_begin` 对 `width<=0` 的 popup 取 `min_w = anchor 宽度`，
本意是给 select 下拉对齐触发器宽，而 context menu 锚是全幅画布。
**修法**：vendored CUI 补一行：`at_mouse` 的 popup 不吃锚宽 floor
（只有 `cui_context_menu_begin` 置 `p->at_mouse`，select/menubar
行为不变）。
**教训**：改 vendored 库前先把参数的真实语义查清——`menu` 形参在这里
是"菜单式条目导航"，不是"右键菜单"；select 也传 menu=true 且需要
锚宽，按 menu 一刀切会回归下拉框。

## L-022 cui_measure_text 的 height 是排版行高，不是字框

**现象**：全部自绘控件（按钮/分段/标题栏/pill/保护条/角标）文字
统一偏高约 3px，和按矩形中心摆放的图标明显不齐；CUI 原生菜单
反而齐。
**修法**：居中用字框 ascent−descent（新增 fl_font_h），9 处全换；
分段控件顺带改成"用哪个字重画就用哪个字重量"（MEDIUM 选中项
曾用 BODY 宽度水平居中）。
**教训**：`cui_draw_text` 的 y 是字框顶（baseline = y + ascent×size），
而 metrics.height = size×1.45 是多行文本流的行盒高——拿行盒高
去居中字框必然偏。CUI 自己组件走内部 cui__font_line_height
(ascent−descent)×size，公开 API 里没有等价助手，自绘代码要
自带这条约定。HarmonyOS 时代同样存在，Noto 的字框/行高差更大
才暴露。

## L-023 IWICBitmapSource.CopyPixels 之前必须钉死输出格式

**现象**：4081×5488 大图去噪完成后预览整体偏色（蓝↔红互换），
落盘 PNG 却完全正确；≤2560 的图预览也正常。
**根因**：`IWICBitmapScaler` 允许改变像素格式——Fant/Cubic/Linear
的 `GetPixelFormat` 如实报告 32bppBGRA（HQCubic 则报告 RGBA），
`img_resize` 没有校验输出格式直接 CopyPixels，BGRA 字节被当
RGBA 建纹理。小图走 `img_thumbnail` 的 memcpy 直拷分支躲过了
缩放器，所以只有大图显形；128px 胶片条缩略图其实全尺寸都反，
只是噪音纹理里没人察觉。
**修法**：缩放器输出套 `IWICFormatConverter`→32bppRGBA（与
img_decode 相同手法），converter 失败时按 GetPixelFormat 报告
做手动 R↔B 交换兜底。
**教训**：任何 `IWICBitmapSource`（Scaler/ColorTransform/Clipper/
FlipRotator…）都可能改变像素格式，CopyPixels 前要么 GetPixelFormat
校验，要么套 FormatConverter 钉格式——"CreateBitmapFromMemory
声明了 RGBA"只约束输入，不约束输出。回归测试：tests/test_img.c
的 test_resize_channels（非对称色块+半透明行，swap 和
premultiply 两种坏法都能抓）。

## L-024 「下载完成」要对齐到消费者的不变量，不是网络事务完成

**现象**：模型管理里点「下载」，`.pth` 实际已写进用户模型目录，按钮却
回弹成「未下载」，模型也永远装不进会话。
**根因**：`eng_model_download` 只把 `.pth` 落进 `user_models_dir`；而
`eng_model_status` 与 `eng_prepare` 都要求 `.skel.onnx` 与 `.pth`
**同目录**（骨架把 `.pth` 当 ONNX external data 按相对路径读，不
同目录 ORT 直接找不到权重）。两个半边各在一处 = 永远不可用——
下载的「成功」定义本身就和消费者脱节。
**修法**：`eng_model_download` 改收 `const eng_config *`（两个目录都
要有），`.pth` 验证落盘后把 `models_dir` 的骨架复制到 `user_models_dir`，
失败即回滚 `.pth` 并明说骨架缺失；`pth_looks_done`（pinned 尺寸 +
PK 魔数）让盘上已完整的权重跳过取网直接补 pair。status 把两半边分开
报，「都存在但不同目录」显示 co-located 缺失而非「未下载」。
**教训**：① 「文件都下载了」≠「功能可用」——成功条件必须对齐到
**加载方真正检查的那个不变量**（这里是同目录 pair），不是对齐到
网络事务返回；② 状态 UI 必须能把「未下载」与「下载了但配不起来」
分开报，否则用户只会看到按钮回弹这种无法解释的表象；
③ 给「下载」加上「已完整则只补缺失半边」的短路语义后，同一个按钮
天然就是修复入口，不用再发明第二个操作。

## L-025 CUI 同名兄弟控件共享一个 id——不是视觉重叠，是同一控件

**现象**：模型管理两行各一个「下载」，悬停一行另一行跟着亮，点击
一次两个模型的下载同时触发。
**根因**：`cui_button_ex` 的 id = `cui_make_id(label)` = `hash(label,
id 栈种子)`。两个同名按钮在同一种子下得到**同一个 id** → 共享
`hot`/`active`/`clicked` 和 `cui__state_lazy` 的动画状态，行为上就是
一个控件画了两遍。
**修法**：每行 `cui_push_id_int` 作用域（CUI 自带 `cui_radio_group`
就是这么隔离同组 radio 的）。
**教训**：循环里渲染同名控件（删除/下载/编辑这类通用动作标签）必须
先推 id 作用域；排查"控件灵异联动"先查 id 撞车再查几何重叠。

## L-026 自绘命中层必须全部过 modal 门控——漏一处就是穿透

**现象**：点设置 sheet 的 × 关抽屉时主窗口的 × 也有反应（sheet × 与
`r_close` 角部重叠 ~20×12px，点击同帧触发 `chrome_close()`）。
**根因**：I13 的 modal 门控只挡住了 stage/overlay/keys 的输入；
`ui_titlebar_input` 在门控之前无条件运行，标题栏的所有 `ui_hit`
hover 读取也都是 modal 盲的。手画命中层不会自动知道 CUI 的弹层。
**修法**：`app_modal_open(st)` 提成 app.h 共享 helper；`ui_titlebar_input`
门控（edge tracker 照喂，防开 modal 的按下留毒）；`titlebar_paint`
7 处 `ui_hit` 读取加 `!inert`。退出路径顺手从 `chrome_close()`
（PostMessage WM_CLOSE，headless 下不可观察）统一到 `want_quit`
（两条路都汇进 `app_shutdown`，bench 可断言）。
**教训**：① 每加一条手画命中/热区路径，先问"modal 开着时它该不该
活"——逐处漏一处就是穿透；② 给 bench 留断言要选可观察的退出
语义（`want_quit` 标志）而不是 Win32 副作用（PostMessage）。

## L-027 对抗性审计两则：所有权移交前先释放；「已在运行」是最坏的谎言

**现象**（二审实锤）：
① `job_apply_done` 直接 `j->res = ev->rgb`——job_rerun 保留
   has_result，每次重跑孤儿化一块全尺寸 RGB（大图数十 MB）。
② join 超时后老线程仍持有任务环（alive=1、stop=1），`dn_start`
   却答「已在运行」→ 启动流程白等 60s 才报假「启动超时」。
   下载任务让这条从理论变成现实（54MB WinHTTP chunk 可超 8s join）。
**修法**：① 赋值前 `eng_image_free(&j->res)`——所有权移交的标准
   姿势是"先释放旧值再收新值"；② `dn_start` 用 `stop` 标志区分
   健康运行 vs 停止中，后者诚实返回 false + last_error，
   `dn_restart` 改返回 bool 让 app 层走现成错误横幅。
**教训**：① 所有权移交的审计模式是"找出所有写 p = new_owner 的点，
   问旧值谁放"；② 状态机里「already up」与「正在死」共用一个
   alive 标志就是谎报源——生死之间必须有一个可判别的中间态。

## L-028 POSIX 移植：bool/errno 两套真值约定，逐一核对

**现象**：APISR 下载明明把 .pth rename 到位，CLI 却报
"cannot rename ... download failed"。
**根因**：`plat_rename` 契约是「返回 true = 成功」（沿用
MoveFileEx 的 bool 语义），移植时误写成 `if (plat_rename(...))`
报错分支——rename 真成功后走错误路径。同类函数
（plat_unlink「删掉了才 true」、plat_read_head、plat_file_size）
与 errno 风（plat_thread_spawn 0=成功）两套约定并存于 plat.h，
逐处核对前很容易写反。
**修法**：`if (!plat_rename(...))`；并把每个 plat_* 返回值约定写进
plat.h 注释——移植期审计清单第一条就是「所有 plat_ 布尔调用点
真值表逐一对照」。
**教训**：跨平台缝把「bool 成功」与「0 成功」两种风格收进同一头
文件时，编译器查不出极性；只能靠契约注释 + 调用点逐一核对 +
端到端跑一次（下载报错的文件其实已就位——输出与报错矛盾即信号）。

## L-029 移植审计：GUI 看不见的隐式前提，CLI 会全踩一遍（2026-11-14）

**现象**：POSIX 移植的对抗审计抓到 6 个真 bug——`--download` 在全新
环境必败（`user_models_dir` 父级不存在）、`--all` 每张图泄漏
`sheet_cells[7]`（释放循环 `i<=7` 但 `i<N_METHODS=7`）、curl 无传输
停滞超时、ORT tar 无 sha256、CMYK JPEG 解成花图、`--device auto`
无说明（spec 与代码不一致）。
**根因**：GUI 路径有隐式准备（denoiser 线程启动时预建模型目录），CLI
调用同一下层 API 却少了这步；循环边界 off-by-one 在 Windows 端也一
直存在只是没人批量跑 `--all` 到显存告警；「WIC 行为」比「stb 行为」
宽的格式（CMYK、0xFF 填充字节）只有逐格式对照才现形。
**修法**：调用方补齐 `mkdir -p`；释放循环独立按实际数组界写；
`--speed-limit/--speed-time` 对齐 WinHTTP 接收超时；tar pinned
sha256；CMYK 复用 `img.c` 朴素公式；auto/gpu 都经 `want_gpu` 提示。
**教训**：移植审计的最有效清单 = 「下层 API 的每个前置条件，哪条是
上层 GUI 悄悄保证的」+「每处 size_t 循环边界对照数组真实大小」+
「每种声称支持的格式真解码一次」。子代理做对抗审读，真机 E2E 补刀。

## L-030 ShellExecute 按扩展名查关联：下载载荷永不走 shell（2026-11-14）

**现象**：「立即更新」点了没反应——安装包已下载校验完，却没拉起安装。
**根因**：载荷落盘叫 `update.bin`，`ShellExecuteExW(verb="open")` 按
扩展名查文件关联；`.bin` 无关联 → `SEE_MASK_FLAG_NO_UI` 把「如何打开」
对话框一并压掉，只剩一条日志。用户视角 = 按钮死了。顺带两个次生洞：
`/RESTARTAPPLICATIONS` 只对安装程序**亲手关闭**的进程有效（本应用是
自己退出的，Inno 不会重启它）；`pending-version.txt` 无条件弹「已更新」
（取消安装后标记残留，下次启动谎报成功）。
**修法**：`CreateProcessW` 直接拉起（不查关联表、per-user 安装本来无需
提权动词）；载荷改名 `update.exe`（万一被 shell/资源管理器触碰也安全）；
命令行 `/autolaunch=1` 配合 .iss `[Run]` 项（`Check: {param:autolaunch|0}`）
在静默装完后把应用拉回来；更新标记只有版本与运行版本一致才计。
**教训**：(1) 执行下载的二进制 = `CreateProcess`，`ShellExecute` 是给
「用户文档」的；(2) 「装完重启应用」要靠 `[Run]+{param:}` 显式握手，
Inno 的 RestartApplications 覆盖不到自己退出的进程；(3) 持久标记必须
与可验证的终态比对，存在性不等于成功。E2E：`bench --update-e2e` +
本地 manifest 可全链路复现（见 test-suite.md 回归门）。

## L-031 颜色断言要覆盖"看得见的角色"，像素对比抓语法层漏网（2026-11-14）

**现象**：深色主题下设置页所有次级文字（说明、路径、未选中分段标签）
渲染成青色；用户截图确诊。浅色主题正常。
**根因**：两处独立病灶叠加——
(1) `apply_fluent_theme` 的 `CUI_TOKEN_TEXT_SECONDARY` 深色值写成
7 位十六进制 `0xCCCCCc7`（= `0x0CCCCCc7`，RGB(12,204,204) 即青色），
本意 `0xCCCCCCC7`——C 编译器不会警告少一位的字面值；
(2) CUI `cui__theme_retarget` 每次重建 stock 目标调色板并 ~320ms lerp
覆盖活跃表，`apply_fluent_theme` 只在明暗翻转时重刷 → 跟随系统/
系统主题切换后调色板静默衰减回 stock（阴性对照：去掉哨兵后深色
accent 落地为 stock `6ba3ff` 而非 Fluent `4cc2ff`）。
**修法**：字面值补位 + `app_frame` accent 哨兵（token 非 Fluent 值即
重刷，无论哪条路径重置）+ bench I19 三 token 断言 + shot 17 深色
设置页截图。
**教训**：(1) 颜色字面值是 8 位 RGBA，少一位=通道全错且编译无警告，
写调色板时对表逐个数位；(2) 断言覆盖面要对齐"用户看得见什么"——
只查 accent 的哨兵在 secondary 全错时照样绿灯；(3) 先验的架构解释
（lerp 覆盖）可能是真的但不是**那个** bug——截图对照才能分清
"颜色错了"和"颜色被覆盖了"；(4) 框架层的渐变/重置机制会把自定义
表洗回默认：自定义调色板必须有自愈哨兵，不能只在"我知道的变化点"
打补丁。
