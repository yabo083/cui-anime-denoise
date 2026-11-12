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

## 进行中

- **chrome**：`WM_SETCURSOR` 缺失导致缩放不可见；最大化 inset 未处理（§3）——ChromeFix 切片。
- **视觉**：外壳瓦片/间距 token 化——ShellRestyle 切片；整体视觉方向由设计切片重新定义
  （产出 `docs/visual-design.md`）。

## 下一步（按依赖顺序）

1. 拆分 `app.c`（2308 行）为 `app.c` / `ui_shell.c` / `ui_stage.c` + `ui_tokens.h`。
2. 切片 A1：异步 worker 启动（`backend.c` + `app.c` 的生命周期部分）。
3. 切片 A2：chrome 命中测试/光标/最大化（`chrome.c`）。
4. 切片 A3：`ui_shell.c` 按 token 重排，瓦片与「添加」同源。
5. 更新基准场景覆盖新不变量（启动期间 UI 可交互、缩放光标区），重跑报告。
