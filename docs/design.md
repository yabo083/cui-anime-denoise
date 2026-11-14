# 设计说明

本文件是**规范**，不是笔记：实现与它冲突时改实现。数字都有来源（美术基准、Windows 指南或实测）。

- 线程与阻塞：§2
- 窗口 chrome：§3
- 视觉系统：§4
- 状态机：§5
- 质量门：§6
- 原生推理引擎：§7

---

## 1. 产品边界

一个 Windows 桌面工具：拖入二次元图 → 去噪 → 局部保护 → 导出。单窗口、单文档流、无工程文件。
**不做**：批处理队列的跨会话持久化、图层次编辑、插件系统、云同步。

复杂度上限：`src/` 每个文件 ≤ 800 行；超出即拆模块（见 AGENTS.md 路由表）。

---

## 2. 线程模型（UI 线程不得阻塞）

### 2.1 三条铁律

1. **UI 线程只做布局、贴图上传和状态迁移**。任何可能超过 8 ms 的工作都不许在帧内同步执行。
2. **推理引擎的启动是异步的**：`dn_start()` 只做 `CreateThread`，立即返回；
   `DN_EV_READY` 通过事件到达，期间 UI 显示启动横幅，任务停在 `QUEUED`。
3. **阻塞调用只允许两类**：原生模态对话框（打开/另存为/选目录）、关窗时的 `dn_shutdown`（有界 join）。
   两者都由用户显式触发或发生在关窗后。

### 2.2 谁在哪个线程

| 工作 | 线程 | 理由 |
|---|---|---|
| CUI 帧、状态迁移、纹理创建 | UI | 纹理句柄只在上传线程有效 |
| 推理、SR 缓存、设备切换、模型下载 | **denoiser 线程**（below-normal） | 引擎调用线程兼容而非线程安全，一个 `eng *` 只允许一条推理 |
| 图像解码（`img_decode_file_ex`） | **加载线程** | 1200 万像素约 36 ms，批量拖入会连坐 |
| 缩略图/显示纹理缩放 | 加载线程 | 与解码同批产出 |
| 蒙版羽化 + 保护合成 + 输出 PNG | **UI**（受控） | 像素已在手里（`j->res`），无 IPC；4096² 羽化约 40 ms，只在笔画结束触发 |
| 更新检查/下载 | 更新线程 | 网络不可控 |

### 2.3 denoiser 状态机

```
        dn_start()
IDLE ─────────────────────► BOOT
                              │  CreateThread 失败 → dn_start false（重试）
                              ▼
                        eng_create + eng_prepare
                              │  DN_EV_READY{ready_ok:1, device, note}
                              ├─────────────► READY
                              │  DN_EV_READY{ready_ok:0, message} → FAILED（横幅 + 重试）
                              │  超时 60 s → 按 FAILED 处理 + dn_shutdown
READY ──(任务内引擎错误)──► WARN(横幅 + 可关闭)
任一状态 ──(重试/换设备)──► dn_restart：shutdown + start，重新发 READY
```

规则：

- 任何状态下 UI 都可交互（拖图、切图、改参数）。
- `QUEUED` 任务只在 `READY` 时下发；`BOOT` 期间不清空队列。
- 超时/失败都要给出**可执行动作**（重试 / 打开日志），不只报错。
- 事件环满时只丢最早的 PROGRESS；终结事件（DONE/ERROR/CANCELLED/READY）永不丢。
- 空闲约 60 s 后 denoiser 线程释放推理会话（`eng_release_sessions`），下一个任务惰性重建。

### 2.4 加载队列

拖入 N 个文件时，只把路径入队；**每帧最多解码 1 张**（≈36 ms 上限），其余显示占位缩略图。
目的：把一次 0.7 s 的冻结摊成 N 帧的轻微停顿，并在解码失败时只影响单张。

---

## 3. 窗口 chrome

自绘标题栏（无原生 caption），由 `src/chrome.c` 子类化 CUI 的窗口过程实现。

### 3.1 命中测试（WM_NCHITTEST）

| 区域 | 返回 | 说明 |
|---|---|---|
| 距任一边 ≤ `8 × dpi` 且未最大化 | `HTLEFT/HTRIGHT/HTTOP/HTBOTTOM` + 四角 | 屏幕坐标判断，不用 ClientToScreen 反复换算 |
| 最大化时四边 | `HTCLIENT` | 最大化不缩放；`WM_NCCALCSIZE` 为最大化返回工作区 inset，避免压到任务栏 |
| `y < 56×dpi` 且 `x < right − 316×dpi` | `HTCAPTION` | 拖动区（品牌 + 空白） |
| `y < 56×dpi` 且靠右 316 | `HTCLIENT` | 按钮区交给 CUI |
| 其余 | `HTCLIENT` | |

### 3.2 光标

`WM_SETCURSOR` 里按命中结果设 `IDC_SIZEWE/SIZENS/SIZENWSE/SIZENESW/IDC_ARROW/SIZEALL`，
返回 `TRUE` 阻止 CUI 把光标强制成箭头——**否则用户看不到可缩放，就会觉得“缩放坏了”**。

### 3.3 其它系统行为

| 行为 | 实现 |
|---|---|
| 双击标题栏最大化/还原 | `HTCAPTION` 由系统处理，不额外写 |
| Aero Snap（拖到边缘吸附） | 依赖 `HTCAPTION` + `WS_THICKFRAME`，自绘时必须保留该 style |
| 圆角 + 阴影 | `DwmSetWindowAttribute(DWMWA_WINDOW_CORNER_PREFERENCE, ROUND)`；`WS_THICKFRAME` 保留阴影 |
| 最大化边距 | `WM_NCCALCSIZE` 在最大化时返回 `MonitorFromWindow` 工作区 inset |
| 深色标题 | `DWMWA_USE_IMMERSIVE_DARK_MODE` 跟随应用主题 |

---

## 4. 视觉系统

布局与视觉的权威规格是 `docs/fluent-redesign.md`（画布优先：40px 标题栏 +
全幅画布，控件全部浮层）。本节是落地层的规矩：

单一来源：`src/ui_tokens.h`（几何/字阶/圆角/动效）+ `src/ui_fluent.c`（规范色
里没有 CUI token 角色的那部分 + 自绘控件原语）。**任何组件不得写字面量圆角/间距**。

### 4.1 结构

| 层 | 内容 | 实现 |
|---|---|---|
| Layer 0 | 40px 标题栏（品牌、文件名链接、设备胶囊、设置/帮助、系统三键） | `ui_shell.c` `ui_titlebar` |
| Layer 1 | 全幅画布：地板（纯色棋盘/毛玻璃，可配置）、对比分屏、遮罩叠加、笔刷光标 | `ui_stage.c` |
| Layer 2 | 浮层：主控 pill(44px)、胶片托盘(68px)、保护工具条(44px)、微调卡(260px)、引擎横幅、空态 | `ui_float.c` |
| Layer 3 | 设置侧栏(320px)、帮助/关于/更新对话框、toast | `ui_shell.c` + CUI 组件 |

浮层全是画布内自绘：绘制帧把命中矩形发布进 `app_state.r_*`，下一帧由
`ui_overlay_input`/`ui_titlebar_input` 消费——输入与绘制永远差一帧，
所以新控件必须先画好矩形再接交互。标题栏的矩形还要过 `chrome_set_zones`
让 `WM_NCHITTEST` 报 HTCLIENT，否则控件区会被当成拖动带。

### 4.2 颜色

1. 有 token 角色的一律走 `cui_token_color`：`apply_fluent_theme`（ui_fluent.c）
   在主题模式翻转时把规范 §4 调色板刷进 CUI token，浅色/深色自动跟随。
2. 没有 token 角色的规范色走 `fl_*`：acrylic 浮层底、卡底、边框、悬停/按下
   状态层、二级/三级文字、遮罩红（浅 `#E81123` α.38 / 深 `#FF4D4D` α.45）。
3. 画面内的中性元素（分屏线、手柄、信息 chip、笔刷环）允许带透明度的
   黑/白字面量——它们是画布覆盖物，不是 UI 皮肤。

### 4.3 一致性规则（评审用）

1. 同一角色的两个控件必须同一实现路径：浮层按钮/图标钮/滑杆/分段全部经
   `ui_fluent.c` 原语，禁止各画各的。
2. 圆角、间距、字号、动效时长只允许来自 `ui_tokens.h`。
3. 图标只允许来自 Lucide（`CUI_ICON_*` 或 `src/icons.h`）。
4. 同一信息不在两处重复（标题条已有文件名，画布内不再写）。

---

## 5. 任务状态机

```
QUEUED ──ready──► RUNNING ──done──► DONE ──(参数变)──► QUEUED
   │                 │  └─error──► ERROR ──(重试)──► QUEUED
   │                 └─cancel───► CANCELLED ──(重试)──► QUEUED
   └─────────────────────────► (BOOTING 期间停留)
```

不变量：

- 同一时刻只有一个 `RUNNING`（denoiser 串行）。
- `DONE` 的 `res_down_pct/res_l0` 记录**产生该结果**的参数；与当前参数不同即提示重跑。
- 蒙版编辑不改状态，只置 `composite_dirty`；合成在宿主侧对 `j->res` 直接做
  （SR 缓存键是 `src_key` + `down_pct`，只影响重跑路径）。
- `ERROR/CANCELLED` 不自动重试。

---

## 6. 质量门

| 门 | 阈值 | 来源 |
|---|---|---|
| UI 帧 p95（各交互场景） | ≤ 1–4 ms | 60 Hz 帧预算的 1/16–1/4 |
| 单张 1200 万像素解码 | ≤ 60 ms | 实测 40 ms |
| 2048² 蒙版羽化 | ≤ 90 ms | 实测 70 ms |
| 笔刷涂抹帧 | ≤ 4 ms | 覆盖层按 40 ms 节流 |
| 交互断言 | 12/12 通过 | `lab/测试工程`（tests/test-suite.md 的 I1–I12） |

每次改 `app.c`/`ui_*.c` 后跑 `lab/测试工程`，报告存档到 `report/latest/`。

---

## 7. 原生推理引擎（`src/engine/`）

引擎是独立的静态库 `anime_engine`：不碰 UI、不碰 denoiser，输入输出都是 RGB8 位图。
对外契约在 `src/engine/engine.h`；本节是**不许违背的不变量**。

### 7.1 线程与取消

- `eng_*` 调用运行在**低于正常优先级**的后台线程；同一个 `eng` 一次只允许一个
  `eng_upscale`/`eng_denoise`（thread-compatible，不是 thread-safe）。
- `cancel` 是调用方自有的 `volatile long`：瓦片循环与 L0 的每个 beta 迭代都检查；
  长调用必须**立即**返回 1，不得等到阶段结束。
- L0 内部自建的工作线程全部 `THREAD_PRIORITY_BELOW_NORMAL`，不得抢 UI。

### 7.2 设备模型

- `AUTO/GPU`：DXGI 高性能首适配器 + DirectML EP（关闭 mem pattern、串行执行）。
  会话建好后先跑 64² 全灰 sanity tile：输出必须有限且落在 [-0.5, 1.5]；
  失败即回退 CPU EP，并置 `eng_note`（与 denoise.py 的 GPU_NOTE 同文案语义）。
- **CUGAN 永远走 CPU EP 整图**：Phase 1 实测 DML 在任意尺寸下输出错误。
- CPU EP：`SetIntraOpNumThreads(物理核数)`，瓦片固定 384。

### 7.3 瓦片方案（与整图推理等价）

- 固定窗口、`step = tile - 2*pad`、`pad = 64`（≥ RRDB 感受野，Phase 1 实测
  pad<64 时边缘出现 ±27 级偏差，pad=64 起 max diff 1）。
- 窗口与图像边缘**齐平**；只在窗口内侧裁 pad —— 边缘窗保留真实边界，
  保持整图推理的 conv 零填充语义。
- tile 大小按**空闲显存**：≥ 6 GB → 512，≥ 3 GB → 384，否则 256。
- 窗口数上限 256/轴；超限必须报错，**禁止**静默输出未覆盖区域。

### 7.4 流水线与数值

`eng_denoise` = BOX 预缩（`down`，默认 0.6，尺寸用 Python round-half-even）
→ 模型 → LANCZOS 回原尺寸 → L0（`l0 > 0` 时）。例外：APISR 不预缩、源图直进；
`ENG_M_NONE` 跳过 SR 只跑 L0。

**平价基准（对 denoise.py，必须保持）：**

| 阶段 | 允许偏差 |
|---|---|
| `eng_resize` BOX/LANCZOS | 与 PIL 8bpc 逐字节一致（fixture 断言） |
| `eng_l0` 同一输入 | 与 float64 numpy 逐字节一致（1024×1536 实测 100% exact） |
| SR 管线（不含 L0） | ±1 级、≥ 99.9% 一致（不同 EP 的 fp 尾差） |
| 全管线（含 L0） | ≥ 99.6% 一致；个别像素 ±7 级以内 |

全管线偏差的机理：SR 输出的 ±1 输入差会让 L0 的 keep-mask 个别翻转，
属预期放大效应，不是实现分歧（同一输入下 L0 逐字节一致可证）。

### 7.5 模型文件

- `esrgan_anime6b_x4.onnx`：权重内嵌，随包进 `<exe>\models\`（`tools/export_onnx.py` 产出，不入库）。
- `*.skel.onnx`：无权重骨架（31–108 KB，**已入库**随仓库分发），把上游 `.pth`
  当 ONNX external data 读；**骨架与 .pth 必须同目录**，查找顺序
  `user_models_dir` → `models_dir`。
- APISR/CUGAN 的 `.pth` 不随包分发：用户下载（`eng_model_download`），
  下载与解包都校验 pinned SHA-256（CUGAN 是 zip，取文件名以 .pth 结尾的成员）。
  下载的成功条件是 **pair 完整**：`.pth` 落盘后随包骨架被复制到同目录，
  骨架缺失则报错并回滚 `.pth`（状态机与 UI 见 §7.6）。
- 会话 I/O 名固定为 `input`/`output`（export_onnx.py 保证）；不一致宁可回退 CPU 也不猜名。

### 7.6 模型管理（GUI 侧功能模块，`src/ui_models.c`）

注册表视角：三个模型统一一张表，每行每帧只读
`eng_model_status()` + `dl_*` 事件槽——状态是文件事实的投影，不猜：

| 状态 | 判定 | 行内表现 |
|---|---|---|
| bundled-ready | `models_dir` 有 `esrgan_anime6b_x4.onnx` | 「已就绪」，无操作按钮 |
| ready | `.skel.onnx` + `.pth` 同目录（user 或 bundled） | 「已就绪」+「删除」 |
| not-downloaded | 无 `.pth` | 「未下载」+「下载」 |
| downloading | `dl_busy[m]` | 进度条（DN_EV_DL_* 驱动） |
| broken-half | 两半边都存在但不同目录 | 「文件不完整」+「下载」（兼作修复）+「删除」 |

- **选择持久化**：`settings.model`（0..2，默认 ESRGAN），「恢复默认设置」归零。
  只能选中 ready 的模型——radio 对不可用项禁用，选不到就是选不到。
- **选择是偏好不是队列参数**：任务出队提交时取当前 `set.model`；
  denoiser 端 `dn_resolve_model` 再落到真实可用模型。文件被删/半截
  状态/载入失败 → 回落 `ENG_M_ESRGAN`，DONE 事件带 `message` 与
  `model`（实际所用模型），UI toast 一次并在结果注记写明——
  **禁止**静默换模型跑。
- **惰性 prepare**：`eng_prepare` 按模型缓存会话（`ses[4]`），换模型无需
  重启引擎；切到新模型的首个任务在任务内付一次编译成本。
- **回退指示是推导的**：`app_model_sel_broken()`（选中模型 pair 不完整即真）
  驱动设备 pill 警示点与设置页警示行——文件回来自动消失，不落字段、
  不会陈旧；`device_note` 保持专属于设备（GPU→CPU）回退。
- **SR 缓存键含模型**：`{src_key, down_pct, model}`——同图同参换模型
  不得复用上一模型的超分结果。
- **下载即修复**：盘上 `.pth` 已满足 pinned 尺寸 + PK 魔数（下载在落盘后
  被中断、或用户手工放置）时，`eng_model_download` 跳过取网只做配对，
  「下载」键天然兼作修复；网络返回 `pth_looks_done` 短路。
- **删除只清 `user_models_dir` 的 pair**；删除当前选中模型时选择同时
  归零回 ESRGAN。CUGAN 的「仅 CPU」在选项描述里明示（§7.2 的硬约束）。

### 7.7 POSIX 移植（`src/plat.h` + `src/posix/`）

CLI 与引擎整体移植到 Linux x86_64，Windows/MSVC 保持主平台。
差异全部收敛在一层平台缝 `src/plat.h`：`eng_path_t`（Windows `wchar_t` /
POSIX UTF-8 `char`）、文件/目录、目录扫描、大小写不敏感比较、原子标志、
`plat_tick_ms`/`plat_num_cpus`、线程 spawn/join、`dlopen/dlsym`。
`src/plat_win.c` 是原有 Win32 语义的搬迁，`src/posix/plat_posix.c` 用
pthread、UTF-8 路径、`/proc/self/exe`、GCC 原子实现同一契约——
`engine.c` / `l0.c` / `models.c` / CLI 因此保持单一代码路径，`#ifdef`
只留在少数真正分歧点（GPU EP 追加方式、回退提示文案）。

GPU 推理在 POSIX 走 ONNX Runtime 的 **CUDA EP**：`eng_create` 建 env 后用
`GetEpDevices` 探测——ORT 只会枚举到 provider 库加载成功且驱动应答的
设备，命中 `CUDAExecutionProvider` 即存下 `OrtEpDevice*`，session 创建时
`SessionOptionsAppendExecutionProvider_V2` 追加；无 CUDA 设备（或 slim
CPU runtime）时整个 GPU 分支不成立，与 Windows DML 失败共用同一条
CPU 回退与提示路径。VRAM 不可经 DXGI 类接口查询，命中 CUDA 时瓦片保守
取 `ENG_TILE_MID`。`setup.sh` 按 `/dev/nvidia*`|`nvidia-smi` 自动选择
`linux-x64-gpu` 包（附带 `libonnxruntime_providers_{shared,cuda}.so`，
tar 有 pinned sha256），`ORT_RUNTIME=cpu|gpu` 可强制。

替换的是**后端**不是逻辑：`img_posix.c` 用 CUI 已 vendor 的 stb 三件套
（decode/write/truetype）复刻 WIC 路径的全部可观察行为——RGBA 解码、
PNG iCCP 与 JPEG APP2 ICC 保留、EXIF 方向、16-bit 按 `v/257+0.5` 缩放、
resize 复用 `engine/resample.c` 同一份 Pillow 移植；`download_curl.c`
用 `curl` 子进程顶替 WinHTTP 的 `upd__download_to_file` 契约（流式进度、
期望尺寸、SHA-256、取消、失败清理）；sheet 标签用 stb_truetype + 同一
Noto Sans SC Medium 字体，字体缺失留空不报错。

无 GPU 的 POSIX 机器上 `want_gpu` 成立但探测为空：打印一行说明、按
CPU 跑、JSON 报告 `"cpu"`（有 CUDA 时报 `"cuda:<adapter>"`）。构建是
`tools/Makefile.posix`（gcc -O2 -Wall -Wextra，零警告），部署镜像与
一键 `setup.sh` 在 `servers/bren9np/anime-denoise/`。
像素级验收：Ubuntu 24.04 CPU 输出对 Windows GPU 参考 RMSE ≤ 1.8e-4，
其中 cugan60/cugan60_l0/l0_* 像素完全一致（纯 CPU-vs-GPU 浮点差异）。
