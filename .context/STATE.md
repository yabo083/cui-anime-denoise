# 当前状态

更新：2026-11-12

## 已交付并验证

- 去噪端到端：拖入/粘贴/命令行 → worker → 对比视图 → 输出 PNG（2096×1586，6.1–6.8 s）。
- 局部保护：笔刷/橡皮/磁力套索、羽化、撤销重做、合成产物 `_protected.png`（实测 8812 像素被还原到原图）。
- 多图队列、缩略图条、快捷键、浅/深色、中文字体（HarmonyOS Sans GB2312 子集）。
- 首次运行装配：uv → Python 3.12 → torch cu130 → 模型下载，实测产出 3.1 GB runtime 并完成一次去噪。
- 安装包：Inno Setup 每用户安装，4.57 MB；静默安装/卸载验证通过。
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

1. **Phase 3**：引擎接入 UI —— `eng_*` 后台线程替代 Python worker 推理路径
   （保留 worker 做合成或直接废弃，按 ADR-007 收口）；`runtime_dir`/`models_dir`
   的安装与打包（35 MB ORT+DML 随包 vs 按需下载）需先决策。
2. 视觉重设计：`docs/visual-design.md`（设计切片产出）→ 按 token 落到 `ui_shell.c`/`ui_stage.c`。
3. chrome：`WM_SETCURSOR` 缩放光标 + 最大化 inset（§3）。
4. 更新基准：启动期可交互（S2）、缩放光标区（W1）纳入自动断言。
