# 当前状态

更新：2026-11-12

## 已交付并验证

- 去噪端到端（原生路径）：拖入/粘贴/命令行 → denoiser 线程 → 对比视图 → 宿主写 PNG
  （2096×1586 ≈ 16 s @esrgan60，保留 alpha 与 ICC，不透明图出 24bpp RGB PNG）。
- 局部保护：笔刷/橡皮/磁力套索、羽化、撤销重做、宿主侧合成产物 `_protected.png`。
- 多图队列、缩略图条、快捷键、浅/深色、中文字体（HarmonyOS Sans GB2312 子集）。
- 安装包：Inno Setup 每用户安装；随包 `runtime/`（ORT+DirectML ≈35 MB）与 `models/`。
- 自动更新：清单 + SHA-256 + 静默升级，本地 HTTP 实测通过/失败两条路径。
- 日志轮转、崩溃报告、诊断包。
- 基准工程 `lab/测试工程`：13 场景 × 1000 次 + 7 项交互断言，总体通过。

## 本轮已完成（2026-11-12 下午）

- `app.c` 拆为 `app.c`（状态机）/`ui_shell.c`（侧栏与外壳）/`ui_stage.c`（画布），
  三个文件都低于 800 行；共享 helper 在 `app.h` 声明。
- 线程：改用 `wb_start_async`，启动不再阻塞；新增 180 s 启动超时与可重试状态。
- 拖入多图：解码队列每帧 1 张，避免整批冻结（`enqueue_path`/`pump_load_queue`）。
- 设置：滑杆改为 500 ms 去抖落盘，不再每帧写盘。
- 画布：分隔线命中区 14px、手柄 hover 放大、笔刷圆环最小 3px、切图重置视图（StagePolish）。
- 视觉 token：`src/ui_tokens.h` 建立；外壳与画布逐步替换字面量。

## 本轮完成（15:20）

- `loader.c`：解码/缩放/缩略图移出 UI 线程；`load_queue` 场景 41 ms → **0.016 ms/帧**，UI 每帧取结果 0.37 ms。
- 拆分收尾：`log.c`（1146）、`update_io.c`（986）、`backend_json.c`（389）；所有文件符合行数规则。
- 视觉：中性滑杆（强调色只在拖动时出现在拇指）、画布底 3% 棋盘、画在图片上的「原图/结果」标签、
  分隔线抓手、分区标题降为次要色、参数移到底部状态行、缩略图条独立表面。
  规范：`docs/visual-design.md`。
- 安装包重建：4.57 MB。

## 本轮完成（18:30，Phase 2 原生引擎）

- `src/engine/` 静态库 `anime_engine` 完成：`engine.c`（ORT 动态装载、DML/CPU EP、
  瓦片推理、流水线）、`resample.c`（Pillow 8bpc 逐字节移植）、`l0.c`（pocketfft
  float64 多线程）、`models.c`（骨架+.pth 状态/下载，复用 update_io 的 WinHTTP+SHA-256）。
- 瓦片方案：固定窗口齐平边缘、pad=64 只裁内侧；tile 按空闲显存 256/384/512；
  256 窗/轴上限超限报错（新增守卫与断言）。
- 设备：DXGI 高性能适配器 → DML EP → 64² 灰块 sanity tile → 失败回退 CPU EP；
  **CUGAN 永远 CPU EP**（Phase 1 实测 DML 输出错误）。
- 逐位平价（对 denoise.py，`tmp/phase2/results.json`）：
  - `esrgan60` 三张图 ×{gpu,cpu}：max diff 1，99.98–99.99% 逐字节一致；
  - `esrgan60_l0`：max 2–7，99.68–99.97%（机理：SR 的 ±1 输入差翻转 L0 keep-mask）；
  - **同一输入的 L0 与 numpy float64 逐字节 100% 一致**（1024×1536、lam 0.001/0.002 均验证）；
  - `cugan60`/`apisr`/`esrgan60` 小图全模型：max 1，≥99.98%（骨架+外部 .pth 加载验证）；
  - 引擎耗时：esrgan60 2080×756 GPU 1.8 s / CPU EP 48 s
    （Phase 1 基线：ORT-CPU 整图 57.7 s、torch CPU 65.4 s）。
- 下载路径实测：APISR 18 MB 直下 + CUGAN 54 MB zip 解包，pinned SHA-256 全部命中。
- 单元测试 `tests/test_engine.c`：120 checks 0 failures；`build.ps1` /W4 零警告。
- 文档：design.md §7（引擎不变量）、ADR-007、AGENTS.md 路由与目录边界。

## 本轮完成（19:05，Phase 3 A+B）

- **Part A 图像平价**（已提交 `efb07e2`）：`img.c` 扩为 `decode_*_ex`（EXIF 方向、
  RGB 系 ICC 提取、16-bit /257 门控缩放、CMYK→sRGB 经内嵌 ICC）+
  `encode_png_icc`（iCCP 写回）；剪贴板移到 `img_clip.c` + `img_priv.h`。
  11 项 fixture 对 PIL `open_image`：ICC 字节全等、灰度 16/EXIF/alpha/CMYK-fallback
  像素全等，48bit RGB/RGBA16 与 CMYK-profile 差异在预期舍入内。
- **Part B 独立 CLI**：`build\anime-denoise.exe`（`src/cli/main.c` 1095 行 +
  `src/cli/sheet.c` 228 行 + CMake 目标），实现 `docs/cli-spec.md` 全量：
  文件/文件夹/`-` stdin、`--stdout` 二进制 PNG、七方法 + `--all` 4x2 sheet、
  `--l0/--down/--device/--models/--download/--json/--quiet`、退出码 0/1/2/3。
- CLI 复用面：anime_engine + img.c/img_clip.c + update_io.c（三个 GUI 钩子
  以本地 stub 满足）；CUI 静态库仅为 `cui__inflate` 链接。
- 验证：31 项参数/错误矩阵全过；真实 `--download apisr` 18 MB + SHA-256 通过；
  stdin→stdout 输出可解码 PNG；文件夹按序枚举并过滤非图片；
  3 图 × 7 方法对 denoise.py 参考：l0-only 100% 逐字节，SR max 1–2，
  链式 _l0 max 2–6、apisr max 3–4——与 `tmp/phase2/out` 驱动器裸输出
  逐位一致（CLI 无额外偏差，差异即引擎既有容差）。

## 本轮完成（Phase 4a 进程内推理取代 Python 侧车）

- `src/denoiser.c/.h`：一条 below-normal 线程独占 `eng *`；任务/事件有界环
  （满时只丢最早 PROGRESS，终结事件不丢）；SR LRU 缓存（2 项，键 = src_key+down_pct）；
  caller-owned cancel；60 s 空闲 `eng_release_sessions`；`dn_start/restart/shutdown`
  全异步，`DN_EV_READY` 回 `ready_ok/device/using_gpu/note`。
- `app.c` 重写推理路径：`dn_push_denoise` 直接递 RGBA 像素（去 IPC）；
  DONE 事件接管 `eng_image` 所有权；宿主侧 `img_composite_masked` 合成保护、
  `img_encode_png_icc` 写盘；`dn_cancel` 取消；READY 超时 60 s → FAILED+重试。
- `ui_shell.c`/`ui_stage.c` 最小改动：健康徽标 → `engine_ready`；设置页引擎段
  （设备 自动/GPU/CPU、重启引擎、模型目录）；runtime 安装面板 → 引擎状态面板。
- `img.c`：`img_composite_masked`（与 worker.py 同公式）；全不透明 RGBA 写 24bpp
  BGR PNG（对 Pillow RGB 输出平价）。
- `config`：`runtime` 目录 → `models`；`settings.device` 0/1/2。
- 构建/打包：CMake 移除 backend/runtime，GUI 链 `anime_engine`；安装包改分发
  `runtime/`+`models/`；`mise.toml` 删 worker 冒烟任务；`worker/`、`assets/python/`、
  `src/backend*`、`src/runtime*`、`docs/worker-protocol.md`、worker 测试全部删除。
- 测试：`test_denoiser` 27 checks、`test_engine` 120、`test_config` 60、
  `test_mask` 85 全过；`test_img` 合成用例过，clipboard CF_DIB/V5 偶发环境性失败。
- E2E：GUI 启动→READY→收图→原生去噪→写盘 ≈16.3 s；无 Python 进程、无 worker.py。

## 进行中

- **chrome**：`WM_SETCURSOR` 缺失导致缩放不可见；最大化 inset 未处理（§3）——ChromeFix 切片。
- **视觉**：外壳瓦片/间距 token 化——ShellRestyle 切片；整体视觉方向由设计切片重新定义
  （产出 `docs/visual-design.md`）。

## 分层结构（CDC）

- `AGENTS.md`：L0 宪法 + 路由表（改哪块读什么）
- `docs/design.md`：规范（线程铁律 / chrome 命中测试 / 视觉 token / 状态机 / 质量门）
- `src/ui_tokens.h`：视觉 token 单源
- `tests/test-suite.md`：L4 行为规格（ID / 步骤 / 预期 / 机制）
- `.context/{DECISIONS,STATE,LESSONS}.md`：架构决策、状态、踩坑
- `lab/测试工程`：基准与交互断言（14 场景 × 1000 次 + 7 断言）

## 下一步

1. **Phase 4b**：视觉重设计 —— `docs/fluent-redesign.md`（未入库草案）→ 按 token 落到
   `ui_shell.c`/`ui_stage.c`；4a 刻意保持 UI 行为不变。
2. `app.c` 已 1251 行，超 ≤800 交互类上限：按职责拆（任务生命周期/合成写盘
   → `job*.c`；启动与设置意图留 `app.c`），拆完在 AGENTS.md 登记。
3. chrome：`WM_SETCURSOR` 缩放光标 + 最大化 inset（§3）。
4. 更新基准：启动期可交互（S2）、缩放光标区（W1）纳入自动断言。
5. `test_img` clipboard CF_DIB/V5 偶发失败是环境性的（多跑几次会通过），
   需要时把 clipboard 用例标为 flaky 并单独重试。
