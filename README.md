# Anime Denoise

原生 Windows 桌面版 GPT 二次元图去噪工具。界面用 [CUI](https://github.com/yappologistic/CUI)（即时模式 UI，C）绘制，
推理交给一个 Python 侧车进程（`worker/worker.py`），去噪算法来自 [gpt-anime-denoise](https://github.com/CXP-2024/gpt-anime-denoise)（MIT）。

所有处理都在本机完成，不上传任何图片。

## 功能

- **去噪**：三档预设（轻柔 80% / 推荐 60% / 强力 45%）+ 预缩比例与 L0 平滑两个滑块；每张图记住自己的参数，
  改了参数会提示「用新参数重新去噪」。
- **局部保护**：笔刷 / 橡皮 / 磁力套索，柔边自动过渡，撤销重做（Ctrl+Z / Ctrl+Shift+Z）。
  涂过的地方恢复原图，其余保持去噪结果；成品另存为 `…_protected.png`。
- **对比视图**：左右可拖动分隔线，`1/2/3` 切换对比 / 原图 / 结果，按住 `Z` 临时看原图，按住 `M` 看保护区域。
- **多图**：拖入、`Ctrl+O` 选择、`Ctrl+V` 粘贴，缩略图条 + 左右方向键切换。
- **缩放**：滚轮以外的 `Ctrl+=` / `Ctrl+-`、`F` 适合窗口、`0` 1:1。
- **输出**：自动写入输出目录，`Ctrl+S` 另存为；透明通道保留。
- 浅色 / 深色 / 跟随系统主题；中文界面，HarmonyOS Sans 字体，Lucide 图标。
- 自更新（清单 JSON + SHA-256 校验 + Inno Setup 静默升级）、日志轮转、崩溃报告、诊断包导出。

## 安装

`dist/AnimeDenoise-<版本>-setup.exe` 是每用户安装（无需管理员），默认装到
`%LOCALAPPDATA%\Programs\AnimeDenoise`。

安装包本身约 4.6 MB：程序、worker 脚本、字体、图标。**AI 运行环境不随包分发**，首次启动时应用会
调用 [uv](https://github.com/astral-sh/uv) 在 `%LOCALAPPDATA%\AnimeDenoise\runtime` 里装 Python 3.12 +
PyTorch（NVIDIA 卡自动选 CUDA 版，否则 CPU 版）+ 去噪依赖，约 2 GB，只需一次。

## 从源码构建

工具链全部由 [mise](https://mise.jdx.dev) 管理，见 `mise.toml`：

```bash
mise install            # python / node / uv
mise run build          # Release，产物 build/AnimeDenoise.exe
mise run run            # 构建并启动
mise run dist           # 打安装包到 dist/（需要 Inno Setup 6 的 ISCC.exe）
```

构建需要 Visual Studio 的 C++ x64 工具集与 PowerShell 7；`build.ps1` 通过 `toolchain.ps1` 自己
捕获 vcvarsall 环境，不要求预置 Developer Command Prompt。

`mise run assets` 从 `_dl/` 里的原始字体重新生成子集（HarmonyOS Sans 的 GB2312 子集、完整 Lucide 图标字体），
`mise run check-fonts` 校验 `src/` 里出现的每个非 ASCII 字符都在 UI 字体里 —— 构建前的守门检查。

## 目录

```
src/
  main.c        界面与状态机（CUI）
  backend.c     侧车进程桥：CreateProcessW + JSONL 协议
  img.c         WIC 解码/编码/缩放/剪贴板
  mask.c        保护蒙版：笔刷、套索、羽化、瓦片撤销、磁力套索
  config.c      settings.json、日志轮转、崩溃报告、诊断包
  runtime.c     uv → Python → PyTorch → 依赖 的首次运行装配
  update.c      清单检查 / 下载 / 校验 / 调起安装包
worker/worker.py 侧车：加载模型、去噪、合成保护区域、模型下载
assets/fonts/    HarmonyOS Sans 子集 + 完整 Lucide（含许可证）
assets/python/   随包分发的 denoise.py（MIT）
installer/       Inno Setup 脚本与打包脚本
tools/           字体子集化与清单生成脚本
tests/           各模块的独立测试程序
```

## 协议

侧车与本程序之间是「一行一个 JSON」的 stdio 协议，见 `docs/worker-protocol.md`。

## 许可

- 本程序：仓库内 LICENSE。
- `assets/python/denoise.py`：MIT，© CXP-2024。
- 字体：HarmonyOS Sans（华为设备有限公司授权，见 `assets/fonts/LICENSE-HarmonyOS-Sans.txt`）。
- 图标：Lucide，ISC。
- UI 框架：CUI，MIT。
- 第三方许可全文随安装包分发在 `assets/` 下。

## 性能与交互基准

性能回归由 `lab/测试工程` 覆盖：13 个场景 × 1000 次迭代，外加 7 项交互断言，
产物是 `lab/测试工程/report/latest/report.md`（含环境、方法、逐场景 p50/p95/p99 与判定）。
它抓到并推动修掉了一个真实问题：笔刷涂抹时每帧重算整张蒙版羽化，帧成本 5.8 ms，
改为 40 ms 节流后降到 0.04 ms。
