# 行为规格（L4）

表格是**规格**：实现与它不符时，先判断是实现错还是规格过时，再改其一。
编号与 `lab/测试工程/bench/bench_main.c` 的断言一一对应；新增行为先加一行，再写断言。
布局依据 `docs/fluent-redesign.md`（画布优先：40px 标题栏 + 全幅画布，控件全部浮层）。

## 交互（`bench_main.c` 的 CHECKS）

| ID | 前置 | 步骤 | 预期 | 机制 |
|---|---|---|---|---|
| I1 | 1 张图 + 假结果 + 保护模式 | 点击保护工具条「橡皮」，再点一次 | `tool` 变 `TOOL_ERASE` 再回 `TOOL_BRUSH` | `r_tool[]` 命中 → 工具状态机 |
| I2 | 同上，工具=笔刷 | 画布上按下并拖动 12 步，点击撤销 | 蒙版出现非零像素；撤销后归零 | 屏幕→图像坐标映射 → `mask_stroke_*` → 瓦片撤销 |
| I3 | 1 张图 + 假结果 | 按 `2` | `view == VIEW_ORIGINAL` | 快捷键表（§8） |
| I4 | 同上 | 点击预设段「轻柔」 | `down_pct == 80`（且触发重跑意图） | pill 分段控件 → 预设表 → `job_rerun` |
| I5 | 8 张图 | 点击胶片托盘第 2 块 | `selected == 1` | `r_queue[]` 命中 → `job_focus` |
| I6 | 1 张图，zoom=3 | 按 `F` | zoom 变为 fit 值（< 1） | 快捷键 → `stage_fit` |
| I7 | 任意 | 切换主题 | `cui_get_theme_mode` 改变且调色板重应用 | `cui_set_theme` → `apply_fluent_theme` |
| I8 | 1 张图 + 假结果 | 按 `B` 再按 `B` | 先进入保护（brush），再完成退出 | `protect_enter`/`protect_done` |
| I9 | 同上 | 开微调卡按 `Esc`；进保护按 `Esc` | 先关卡，再退保护（逐层） | §8 Esc 递进 |
| I10 | 同上，蒙版已涂 | 按 `I` 再按 `Ctrl+Z` | 反选后非零像素大幅增加；撤销恢复原笔画 | `mask_invert` 一步入册 |
| I11 | 8 张图 | 悬停托盘第 2 块 → 点击 × | `count` 减 1 | `r_tray_x[]` 悬停发布 → `job_remove` |
| I12 | 1 张图 + 假结果 | 按 `X` 两次 | 对比 → 原图 → 结果 | §8 X 互切 |
| I13 | 同上 + 设置侧栏打开 | 按 2/X/B、点画布分隔线；关栏后再按 2；涂抹中途开栏 | 栏开时键鼠不穿透（view/protect/分隔线不变），点击只关栏；关栏后按键恢复；被中断的笔画完整收尾 | `app_frame` 模态门：`modal_open` 时跳过 `app_handle_keys`/`ui_overlay_input`/`stage_handle_input` 并收拢拖拽态 |
| I14 | 同上（结果就绪） | 右键画布；菜单开着时按 2；`Esc` 后再按 2；`Ctrl+C` / `Ctrl+Shift+C` | 右键开画布菜单（`ctx_open`）；开着时按键不穿透；Esc 关闭且按键恢复；`Ctrl+C` 写入合成结果（`job_output_px`，含保护合成），`Ctrl+Shift+C` 写入原图——剪贴板回读逐字节校验 | `modal_open` 含 `ctx_open`；保护模式下不弹菜单（右键是临时橡皮手势） |
| I15 | bench 模型目录（预置 bundled onnx + 骨架，用户目录为空） | 打开设置侧栏 | 模型节渲染：`dn_model_status(ESRGAN)` 就绪、CUGAN/APISR 未下载 | `ui_models_section` 每帧读 status + `dl_*` |
| I16 | 用户目录放 `.pth`、骨架只在 bundled（分离 pair） | `eng_model_pair_complete` | 分离态 `!available` 且 `missing` 指明不共址（按钮呈「下载」）；补全后 `available`（按钮消失呈「已就绪」），`dn_resolve_model` 返回该模型 | 同目录 pair 不变量（§7.6）；补全=下载的收尾动作 |
| I17 | CUGAN pair 已就绪 | `app_select_model`；`cfg_load` 回读；`app_delete_model`；把 `set.model` 置成无文件模型 | 不可用模型选择被拒；选中持久化进 settings.json；删除选中模型后 `set.model` 归零；失效选择经 `app_model_sel_broken` 派生警示、`dn_resolve_model` 回退 ESRGAN | 选择是偏好（§7.6）：提交时 resolve，不可用回退不静默 |
| I18 | 设置侧栏打开 | 点击 `r_close` 中心；侧栏消失后再点同一像素 | 第一次：sheet 被 scrim 点击关闭但 `want_quit` 不置位（标题栏整条惰性）；第二次：`want_quit` 置位（按钮不死） | `app_modal_open` 门控 `ui_titlebar_input` + `titlebar_paint` 的 `ui_hit` 读取；sheet 的 × 与 `r_close` 存在 ~20×12px 角部重叠，裸跑会把关窗穿透成 `chrome_close()` |

## 启动与线程（需人工或新增场景验证）

| ID | 前置 | 步骤 | 预期 | 机制 |
|---|---|---|---|---|
| S1 | 重命名 `runtime/`（缺 ORT/DirectML DLL） | 启动应用 | 画布顶部浮出错误横幅 + 重试按钮，UI 可交互 | `DN_EV_READY{ready_ok:0}` 分支 |
| S2 | 引擎正常 | 启动 | 启动横幅出现且不挡输入，任务保持「排队中」；READY 后可跑 | `dn_start` + BOOT 状态机 |
| S3 | 推理中 | 同时拖入 20 张图 | 单帧 UI 成本 < 1 ms（解码在加载线程） | `loader.c` 线程 + 每帧取一个结果 |
| S4 | 推理中 | 拖动滑杆 2 秒 | `settings.json` 只在停止 500 ms 后写一次 | 设置去抖 |
| S5 | 任意 | 关闭窗口 | 设置落盘、denoiser 线程退出、日志正常结束 | `dn_shutdown` |
| S6 | GPU 可用但 DML 失败 | 启动引擎 | 标题栏设备胶囊变警告点并带三角标；设置页显示回退说明 | `DN_EV_READY.message` → `device_note` |

## 窗口 chrome

| ID | 前置 | 步骤 | 预期 | 机制 |
|---|---|---|---|---|
| W1 | 未最大化 | 指针移到窗口左边缘 | 光标变横向缩放，拖动可改变宽度 | `WM_NCHITTEST` HTLEFT + `WM_SETCURSOR` |
| W2 | 未最大化 | 拖动标题栏空白处 | 窗口移动；拖到屏幕边缘可吸附 | `HTCAPTION` + `WS_THICKFRAME` |
| W3 | 未最大化 | 双击标题栏 | 最大化；再次双击还原 | 系统对 `HTCAPTION` 的默认处理 |
| W4 | 已最大化 | 点击「还原」 | 回到之前的尺寸与位置 | `chrome_toggle_maximize` |
| W5 | 已最大化 | 观察窗口与任务栏 | 客户区不覆盖任务栏 | `WM_NCCALCSIZE` 返回工作区 inset |
| W6 | 任意 | 点击右上角三个按钮 | 最小化 / 最大化 / 关闭分别生效，且不与拖动区冲突 | 右侧 `UI_CAPTION_BUTTONS` 区返回 `HTCLIENT` |
| W7 | 未最大化 | 指针停住不动，停留在左边缘 3 秒 | 缩放光标**不闪**，拖动全程形态不变 | `WM_SETCURSOR` 只用命中码；capture 期间锁存光标 |
| W8 | 画布上有图 | 滚动滚轮 | 以光标处图像点为锚缩放，0.02–16 倍；保护模式下空格+滚轮调笔刷 | `chrome_take_wheel()` + 锚定公式 |
| W9 | 任意 | 指针悬停标题栏的 关闭 按钮 | 按钮染 Fluent 红 `#C42B1C`，窗口不进入拖动 | `chrome_set_zones` 把控件区标为 HTCLIENT |
| W10 | 任意 | 点击标题栏的文件名 | 资源管理器 `/select` 打开源图 | `app_reveal_in_explorer` |

## 视觉一致性（人工核对，截图存档）

| ID | 检查 |
|---|---|
| V1 | 主控 pill：44px 胶囊、亚克力底、距画布底 24px、居中；无图时只有预设+「添加图片」 |
| V2 | 胶片托盘：仅当 >1 张出现，68px、位于 pill 上方 8px；选中瓦片 2px 强调色描边，悬停出 × |
| V3 | 保护工具条：进入时从顶部滑入并替换 pill；工具/三滑杆/撤销重做/眼睛/反选/清空/未保护/取消/完成 全到齐 |
| V4 | 对比分屏：2px 白分割 + 28px 圆形手柄（悬停 +10%）；±8px 命中带；边缘 24px 内松手吸附 |
| V5 | 遮罩叠加色：浅色 `#E81123` α≈0.38 / 深色 `#FF4D4D` α≈0.45；笔画后 ~0.7s 淡出，眼睛可钉住 |
| V6 | 标题栏：40px，左品牌+文件名链接，右设备胶囊+[更新]+?+⚙+系统三键（46px 宽、关闭悬停红） |
| V7 | 微调卡 260px 宽、空态卡 380×196、设置侧栏 320px；浮层进出 ~200ms |
| V8 | 浅色/深色两套主题下正文对比度 ≥ 4.5:1；强调色 `#0067C0`/`#4CC2FF` |
| V9 | 托盘瓦片 48px 圆角 6，分页器仅在超页时出现 |
| V10 | 大图预览通道序：shot 16（2800×1900 > DISPLAY_MAX）对比分屏两侧色相一致——WIC 缩放器可能输出 BGRA，两侧预览均经 `img_resize`，R↔B 交换会立即可见；字节级回归由 `test_img.c::test_resize_channels` 保证 |
| V11 | 模型节行隔离：两行都有「下载」/「删除」时，悬停其一**只有该行**高亮，点击只触发该行（同名按钮须在各行的 `cui_push_id_int` 作用域内；bench 里按钮禁用，人工核对） |

## 回归门

`lab/测试工程` 跑 1000 次迭代后：14 个性能场景 p95 均在预算内、18 项交互断言全 PASS、
`report/latest/report.md` 存档。引擎层模型配对/下载语义另由 `tests/test_models.c`
（无网络，47 断言）与 `tests/test_denoiser.c`（真实引擎 39 断言：模型回退 +
`dn_start` 白盒判别 alive/stop——join 超时孤儿窗口里必须拒答而非谎报"已就绪"）覆盖。
任务结果所有权另由 `bench --rerun-e2e`（真实引擎连续重跑，私有字节增长 ≤ 一个结果缓冲）
守住 `job_apply_done` 的释放顺序。

POSIX（`tools/Makefile.posix`）侧另有人工验收：Ubuntu 24.04/gcc 13.3 零警告构建；
文件/文件夹/stdin/stdout/JSON/`--all` sheet/EXIF 方向/ICC 往返/16-bit 输入/模型
下载（zip+pth、SHA-256、pair 完整）/SIGINT 取消/`--device gpu` 回退逐项验证；
七种方法输出对 Windows GPU 参考 RMSE ≤ 1.8e-4（cugan60/cugan60_l0/l0_* 像素一致，
差异为 CPU-vs-GPU 浮点），部署镜像 `servers/bren9np/anime-denoise/` 的 `setup.sh`
从零重建可复现同一结果。
