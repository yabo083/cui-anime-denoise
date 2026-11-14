# Changelog

本项目版本与 GitHub Release 一一对应；GUI 内自动更新读取 Release 资产
`update.json`（`releases/latest/download/` 始终指向最新版）。

## [1.1.7]

### 修复
- 「立即更新」点了没反应：下载的安装包落盘叫 `update.bin`，而启动它走
  `ShellExecute(open)`——按扩展名查文件关联，`.bin` 无关联直接静默失败
  （`SEE_MASK_FLAG_NO_UI` 把「如何打开」也压掉了）。改为 `CreateProcessW`
  直接拉起，不再依赖关联表；载荷改名 `update.exe`。
- 静默装完不会把应用拉回来：`/RESTARTAPPLICATIONS` 只对安装程序亲手关闭
  的进程有效，而本应用是自己退出的。安装脚本新增 `[Run]` 项，由安装命令
  行的 `/autolaunch=1` 触发，装完自动重新打开。
- 更新标记 `pending-version.txt` 无条件弹「已更新」：安装被取消/回滚后
  标记残留，下次启动会谎报更新成功。现在只有标记版本与运行版本一致才计。

## [1.1.6]

### 新增
- `anime-denoise` CLI 与推理引擎移植到 Linux x86_64：
  `tools/Makefile.posix` 无 CMake 构建（gcc -O2 -Wall -Wextra -pthread
  零警告），运行时 `dlopen` `libonnxruntime.so`。
- POSIX GPU 推理：ONNX Runtime **CUDA EP**。`eng_create` 经
  `GetEpDevices` 探测 `CUDAExecutionProvider` 设备（命中即驱动+provider
  可用），session 用 `AppendExecutionProvider_V2` 追加；无 CUDA 时
  `--device gpu|auto` 一行说明回退 CPU、JSON 报 `"cpu"`，有 CUDA 时报
  `"cuda:<adapter>"`（DirectML 仍为 Windows 专属，行为不变）。CUGAN
  与 Windows 一致固定 CPU。
- `setup.sh` 按 `/dev/nvidia*`|`nvidia-smi` 自动选择 ORT
  `linux-x64(-gpu)` 包（`ORT_RUNTIME=cpu|gpu` 强制），tar 有 pinned
  sha256 校验，.so 原子暂存。
- Linux 终端安装包（`tools/build_pkg.sh`，Inno Setup 对位物）：
  `anime-denoise_<ver>_amd64.deb`（`/usr/lib/anime-denoise` + `/usr/bin`
  软链 + man 页 + copyright，`dpkg -i/-r` 托管）与
  `anime-denoise_<ver>_linux-x64.tar.gz`（便携树 + `install.sh
  --prefix`）；NVIDIA 机器 `sudo anime-denoise-cuda-setup` 原地换装
  CUDA provider 库。
- 新增平台缝 `src/plat.h`（路径/文件/目录扫描/线程/原子/时钟/dlopen），
  实现分 `src/plat_win.c`（原 Win32 语义）与 `src/posix/plat_posix.c`；
  `engine.c`/`l0.c`/`models.c`/CLI 共用单一代码路径。
- POSIX 图像后端 `src/posix/img_posix.c`（复用 CUI vendor 的 stb）：
  保留 alpha、PNG iCCP、JPEG APP2 ICC、EXIF 方向与 16-bit 缩放；
  `--all` sheet 标签经 stb_truetype + Noto Sans SC Medium 渲染。
- POSIX 模型下载 `src/posix/download_curl.c`：curl 子进程、流式进度、
  pinned SHA-256、CUGAN zip 成员（stored/raw-DEFLATE）提取、失败清理，
  遵循 `http(s)_proxy`；pair 完整语义与 Windows 一致。
- 部署镜像 `servers/bren9np/anime-denoise/`（`setup.sh` 一键：
  下载 ONNX Runtime 1.24.4 → 构建 → 摆放 runtime/models/assets）。

### 修复
- `--download` 在全新环境失败：`user_models_dir` 的父级不存在时 curl
  无法落盘（CLI 两侧平台都中；GUI 因 denoiser 预建目录未中）。CLI 现在
  下载前 `mkdir -p`。
- `--all` 每张图泄漏一个 RGBA 缓冲：`sheet_cells[7]`（第七种方法结果）
  释放循环边界差一。
- POSIX 下载中途无限挂起：curl 仅有 `--connect-timeout`，补上
  `--speed-limit/--speed-time` 传输停滞超时（对齐 WinHTTP 接收超时）。
- CMYK JPEG 解码成花图：stb 返回原始 CMYK 通道，现按 WIC 朴素回退
  `255-min(255,C+K)` 转换、alpha 置不透明。
- JPEG marker 扫描不跳过 0xFF 填充字节（ICC/EXIF 提取在带填充的文件上
  失同步）；ICC APP2 去重 `seq_no`，杜绝重复段拼出错误 profile。
- `plat_thread_join` 缺 NULL 守卫（对齐 `plat_win`）；`o->inputs` 与
  单文件 `files` 的 calloc 未判空；`--` 结束选项标记；>2 GiB 文件读入
  提前失败（stb 长度是 int）。
- 构建输出零警告：CMP0092 去掉 CMake 注入的 `/W3`（此前每个 TU 都报
  D9025），vendored CUI 的 C4457 按目标静默（与 pocketfft 的 /wd 同法）。

## [1.1.5]

### 新增
- 处理结果可复制到系统剪贴板：`Ctrl+C` 复制与「另存为」一致的合成结果
  （保护蒙版生效时复制的是羽化合成后的可见结果），`Ctrl+Shift+C` 复制
  原图；走既有 `img_clipboard_set`（CF_DIBV5 + PNG 双格式，对 Snipaste/
  画图/浏览器粘贴均有效）。
- 画布右键菜单（CUI 原生 `cui_context_menu`，锚定画布节点）：复制结果 /
  复制原图 / 粘贴图片 / 打开图片 / 另存为 / 适合窗口 / 实际大小 1:1 /
  设置 / 帮助。保护模式下不弹出——该模式下右键是临时橡皮手势。
- 模型管理升级为完整功能模块（设置 → 模型）：内置 ESRGAN 与已下载的
  CUGAN/APISR 三个模型一张注册表，单选即切换，选择持久化
  （`settings.model`），对尚未开始的任务即时生效；每行带状态（就绪 /
  未下载 / 文件不完整 / 下载进度）与下载、删除操作，CUGAN「仅 CPU」
  的硬约束写在选项描述里。换模型无需重启引擎——会话按模型惰性编译。
- 所选模型文件失效（被删、半截下载、载入失败）时任务自动回落内置
  ESRGAN：设备 pill 亮警示点、设置页写明原因、结果注记与 DONE 事件
  注明实际所用模型——绝不静默换模型跑；文件补齐后警示自动消失。

### 修复
- 模型管理「下载后按钮回弹未下载」：下载只把 `.pth` 落进用户模型目录，
  无权重骨架却留在安装目录——引擎与状态检查都要求骨架与 `.pth`
  **同目录**成对（骨架按相对路径读 external data），于是下载成功了
  状态仍「未下载」、按钮复原，模型也永远装不进会话。现在下载以
  「配对完成」为成功条件：`.pth` 验证落盘后随包骨架被复制到同目录；
  骨架缺失报错并回滚 `.pth`，不留半截状态。盘上已有完整 `.pth`
  （下载中断、手工放置）时点「下载」跳过网络只补骨架，兼作修复。
- 超分结果缓存键未含模型：同图同参数换模型会误复用上一模型的结果；
  键改为 `{src, down, model}`。
- 右键菜单视为模态层：`ctx_open` 并入 `modal_open` 门，菜单开着时
  快捷键与画布点击不再穿透到画布（否则会与 I13 修过的穿透同类问题）。
- 右键菜单曾被拉成画布全宽：vendored CUI 给未指定宽的 popup 兜底
  「min_w = 锚点宽」（本意是 select 下拉对齐触发器），at-mouse 的
  context menu 不该吃这条；已修，对 select/菜单栏无影响。
- `Ctrl+C` 无结果时不再静默回落复制原图，改为提示「还没有可以复制
  的结果」——复制结果就复制结果，不可见的替换是欺骗。
- 自绘控件文字普遍偏高 ~3px：之前按 `cui_measure_text().height`
  （1.45×字号的排版行高）垂直居中，但 `cui_draw_text` 的 y 是字框顶，
  两值不等即偏移；现统一用字框（ascent−descent，与 CUI 内部
  `cui__font_line_height` 同口径，见 `fl_font_h`），按钮/分段控件/
  标题栏/设备 pill/保护条数值/角标/笔刷提示共 9 处全部归位，图标与
  文字回到同一中线。
- 浮层控件宽度全部改为内容自适应：`ui_button_w`/`ui_seg_w` 按
  MEDIUM 字重实测标签宽，按钮内容在矩形内居中（`ui_button_draw`），
  分段控件每段=最宽标签+双侧 12px 内边距（单段下限 48px）；pill 与
  保护工具条的容器宽=子控件实测宽之和，不再出现固定常量撑出的
  右侧死区。滑杆轨道与数值区仍定宽——拖数值时整条工具条不应抖动。
- 设置侧栏的 × 点击穿透到主窗口关闭钮：sheet 的 × 与 `r_close` 角部
  重叠，标题栏命中层不走 modal 门控——点 sheet 的 × 会同帧触发
  `chrome_close()`；modal 打开时标题栏输入与悬停视觉全部惰性化，
  退出统一走 `want_quit`（bench 可断言）。
- 模型管理两行「下载」/「删除」按钮互相联动：同名按钮在同一 id 种子
  下解析出同一个 CUI id，共享悬停/按下/点击状态——悬停一行另一行跟着
  亮，点击一次两个模型同时下载；每行加 `cui_push_id_int` 作用域后隔离。
- 分段控件选中拇指与槽位错位：拇指节距误用 `iw - inset` 而非 `iw`，
  第 2 段偏左 4px、第 3 段偏左 8px——选中「结果」时拇指明显脱离文字
  槽位；已改为 `iw` 节距，各端 inset 对称。
- 胶囊容器内的控件改为同心圆角：pill / 保护工具条 / 横幅内的分段控件
  轨道与拇指、图标钮、文本按钮一律胶囊（内层半径 = 容器半径 − 内边距，
  32px 高轨道即 R16、图标钮成正圆），不再出现胶囊套小方角的违和感；
  卡片容器内的控件维持 MD 圆角。`ui_iconbtn_draw` / `ui_button_draw` /
  `ui_seg_draw` 新增 radius 参数把这条约定写进签名。
- 大图（长边 > 2560）预览整体偏色（蓝↔红互换）：`IWICBitmapScaler`
  允许改变输出像素格式——Fant/Cubic/Linear 模式经 `GetPixelFormat`
  如实报告输出 BGRA，`img_resize` 却未校验格式直接 `CopyPixels`，
  把 BGRA 当 RGBA 交给纹理。落盘文件正确（编码路径不经缩放器），
  ≤2560 的图也正常（`img_thumbnail` 走 memcpy 直拷），故该问题只在
  大图预览出现。修复：缩放器输出经 `IWICFormatConverter` 钉回 RGBA
  （与 `img_decode` 同款手法），converter 不可用时按 `GetPixelFormat`
  报告手动交换兜底。波及面：原图显示、结果预览、胶片条缩略图
  （>128px 的图都反）、剪贴板导入缩放——一处处同根同愈。
- 重跑任务泄漏上一次的全尺寸结果缓冲：`job_apply_done` 直接覆盖
  `j->res`（job_rerun 保留 has_result），每次参数微调孤儿化一块
  全尺寸 RGB（大图数十 MB/次）；赋值前释放旧缓冲。
- 推理引擎在下载等大粒度任务中被重启时假死 60 秒：join 超时后老线程
  仍持有任务环（alive=1、stop=1），`dn_start` 却回答「已在运行」，
  启动流程白等一个不会来的 READY 才报「启动超时」。现在 `dn_start`
  凭 `stop` 标志区分健康运行与停止中，诚实返回「引擎还在停止中，
  请稍后重试」并在横幅呈现——孤儿线程退出后重试即恢复。

## [1.1.4]

### 修复
- 中文字体从 HarmonyOS Sans 换成 Noto Sans SC（思源黑体同族字形，SIL OFL
  1.1）：HarmonyOS 许可只允许再分发「未修改副本」，而我们的随包字体是
  GB2312 子集——属条款禁止的修改。OFL 明确允许子集化与嵌入，改用可变
  字体 instancer 出 400/500 两档再子集；观感与字阶不变，体积 2.1MB。
- `THIRD-PARTY-NOTICES.txt` 补全许可证名称：Real-CUGAN 权重标注 MIT、
  APISR 权重标注 GPL-3.0（后者正是骨架图 + 按需下载方案存在的原因）。

## [1.1.3]

### 新增
- 画布底支持「毛玻璃」（默认）：最近一次输出缩至 ~160px、六次盒式模糊后
  盖 80% 主题色纱，类 Mica 动态取色；无输出时回落纯色棋盘。设置 →
  「画布背景」可切回纯色（持久化 `canvas_bg`）。
- 应用内自动更新接入 GitHub Release：默认清单地址指向
  `releases/latest/download/update.json`，随每个版本发布；
  `ANIME_DENOISE_UPDATE_URL` 环境变量仍可覆盖做本地联调。

### 修复
- 设置抽屉/帮助/关于/更新对话框打开时输入穿透：画布分隔线可拖、浮层
  控件可点、快捷键照走。现在模态遮罩独占键鼠（拖放文件仍收，属于
  窗口级意图），被中断的笔画完整收尾，关层后输入即恢复。
- 修复模态开启时被按住的按键沿让各层 `was` 边沿跟踪器失效、关层后
  第一次点击被吞的问题（门控期间继续喂跟踪器，不透传语义）。
- `WM_MOUSEWHEEL` 被子类独吞导致 CUI 滚动视图（含设置抽屉）无法滚动；
  现在累加给画布缩放的同时也转发给原窗口过程。

## [1.1.2]

### 修复
- 拖动窗口边缘时右侧/底部留白：主循环手动跑帧未注册 frame 回调，OS
  模态缩放期间只能重播旧尺寸帧；注册后随拖随建。
- 保护工具条右侧裁切约 14px、「已保存」tick 溢出 8px：改为一次 x-walk
  同时测量与绘制，宽度不再手写求和。
- 磁性套索已提交段是直线、只有预览贴合：改为与原 web 版同构的密集
  折线模型（`L.pts` 式），commit 把整条吸附 wire 入轮廓，闭合时回起点
  也走 live-wire，Backspace 按锚点整段回退。
- exe 的 VERSIONINFO 曾停留在 1.1.0：改由 CMake `PROJECT_VERSION`
  生成。

### 新增
- 「另存为」默认文件名可配置：设置 → 保存文件名，支持
  `{app} {src} {date} {time} {suffix}` 变量，默认 `{app}_{date}-{time}`。

## [1.1.1]

### 修复
- 画笔/橡皮/磁性套索在真实加载路径下全部失效：`job_add_decoded` 在移交
  `r->full` 后读到的是已清零尺寸，蒙版层从未分配，保护模式静默拒绝进入。
  端到端测试改走真实加载队列并断言蒙版已分配。
- 快捷键帮助把 `/` 渲染成独立键帽：替代键拆成分立键帽、以「或」相连，
  纯手势改渲染为文字。
- 加载圈/旋转弧卡顿：手绘弧只按墙钟推进却不请求下一帧，事件驱动的
  帧调度停下后弧只在有输入时跳格；新增自请求重绘的 `fl_spinner`。
- 窗口边缘缩放光标被踩回箭头：自绘 chrome 与 CUI 后端双重写光标；
  现在 `WM_SETCURSOR` 只认领非客户区命中码，CUI 只在 `HTCLIENT`
  时应用 widget 光标。

## [1.1.0]

首个原生重写版：C + CUI 即时模式 UI、进程内 ONNX Runtime（DirectML/CPU）
推理、独立 `anime-denoise` CLI、Inno Setup 安装包。功能对齐
`gpt-anime-denoise` 的 `denoise.py`（ESRGAN/CUGAN/APISR + L0、局部保护、
对比视图、队列托盘），无需 Python。
