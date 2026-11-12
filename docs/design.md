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

单一来源：`src/ui_tokens.h`。**任何组件不得写字面量圆角/间距**，一律引用 token。

### 4.1 尺寸与圆角

| token | 值 | 用途 |
|---|---|---|
| `UI_R_CONTROL` | 8 | 按钮、输入框、滑轨 |
| `UI_R_TILE` | 12 | 缩略图瓦片、工具按钮（方形） |
| `UI_R_CARD` | 14 | 队列卡片、面板 |
| `UI_R_DIALOG` | 18 | 对话框 |
| `UI_PAD` | 12 | 面板内边距 |
| `UI_GAP` | 8 | 同组元素间距 |
| `UI_TILE` | 62 | 缩略图与「添加」瓦片共用 |
| `UI_TOPBAR` | 56 | 标题栏高度（与 chrome 命中区一致） |
| `UI_SIDEBAR` | 300 | 侧栏宽度 |
| `UI_STRIP` | 92 | 缩略图条高度 |

### 4.2 字号

| token | 值 | 用途 |
|---|---|---|
| `UI_FS_CAPTION` | 10.5 | 路径、次要说明 |
| `UI_FS_BODY` | 12 | 正文、按钮 |
| `UI_FS_LABEL` | 12.5 | 分组标题 |
| `UI_FS_TITLE` | 14–16 | 文件名、品牌 |

### 4.3 颜色

只用 CUI 主题 token（`CUI_COLOR_*`），不写十六进制。语义映射：

| 语义 | token |
|---|---|
| 窗口底 / 面板 | `BACKGROUND` / `SURFACE` |
| 次级面板、输入底 | `SURFACE_MUTED` |
| 选中态描边、主按钮 | `ACCENT` / `ACCENT_STRONG` |
| 完成 / 出错 / 警告 | `SUCCESS` / `DANGER` / `WARNING` |

例外只有一个：蒙版浮层的珊瑚色 `224,122,95` 与棋盘格，它们表达的不是主题语义，而是图像内容叠加层。

### 4.4 组件规格

**缩略图瓦片（含「添加」瓦片）** — 同尺寸、同圆角、同描边，同一绘制函数 `tile_paint`：

```
方框 62×62，圆角 12，内缩 1px 画图，描边 1（选中 2 + ACCENT）
「添加」瓦片：同样的框，中间画 20px 加号，描边色 BORDER，hover 时 ACCENT
```

两者由**同一个绘制函数**产出，禁止一个用按钮、一个用画布。

**按钮**：高 30（SM）/36（MD），圆角 8，图标 14，文字 12。
**卡片**：圆角 14，内边距 7，行间距 8；hover 抬升 1px，外层滚动区留 8px 余量免被裁剪。
**分段控件**：外框圆角 8，内块圆角 6，高 26。
**滑块**：轨高 3，拇指 12，标签 12.5。

### 4.5 一致性规则（评审用）

1. 同一角色的两个控件必须同一实现路径（瓦片+加号、两个分段控件、两种卡片）。
2. 圆角、间距、字号只允许来自 §4.1/§4.2。
3. 图标只允许来自 Lucide（`CUI_ICON_*` 或 `src/icons.h`），尺寸 14/16/18/20。
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
| 单张 1200 万像素解码 | ≤ 40 ms | 实测 36 ms |
| 2048² 蒙版羽化 | ≤ 60 ms | 实测 18 ms |
| 笔刷涂抹帧 | ≤ 4 ms | 覆盖层按 40 ms 节流 |
| 交互断言 | 7/7 通过 | `lab/测试工程` |

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
- `*.skel.onnx`：无权重骨架，把上游 `.pth` 当 ONNX external data 读；
  **骨架与 .pth 必须同目录**，查找顺序 `user_models_dir` → `models_dir`。
- APISR/CUGAN 的 `.pth` 不随包分发：用户下载（`eng_model_download`），
  下载与解包都校验 pinned SHA-256（CUGAN 是 zip，取文件名以 .pth 结尾的成员）。
- 会话 I/O 名固定为 `input`/`output`（export_onnx.py 保证）；不一致宁可回退 CPU 也不猜名。
