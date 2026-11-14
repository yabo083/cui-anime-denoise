# anime-denoise CLI 规格

随安装包分发的命令行工具 `anime-denoise.exe`（console subsystem），与 GUI 共享 `anime_engine`。
目标：覆盖 `denoise.py` 全部能力 + 融入脚本管线（stdout/JSON/退出码/stdin）。

## 用法

```
anime-denoise <input...> [options]
  input     图片文件或文件夹（文件夹取全部支持格式的图）；"-" 表示从 stdin 读图片
options:
  --out DIR           输出目录（默认 <图片目录>/<文件名>_denoise/）
  --method NAME       esrgan60_l0(默认) esrgan60 cugan60_l0 cugan60 apisr l0_0.001 l0_0.0005
  --all               跑全部 7 种 + 4x2 对比图 <stem>_sheet.png
  --l0 X              L0 强度（默认 0.001；0 = 跳过；必须有限且 >= 0，否则 usage error）
  --down PCT          预缩比例（默认 60；apisr 忽略）
  --device auto|gpu|cpu   推理设备（默认 auto）
  --models DIR        可选模型目录（默认 %LOCALAPPDATA%\AnimeDenoise\models）
  --download NAME     下载 CUGAN/APISR 上游权重到 --models（校验 SHA-256）后退出
  --json              stdout 每完成一项输出一行 JSON {"input","output","method","w","h","ms","device"}
  --quiet, -q         不打进度
  --stdin             从 stdin 读一张图
  --stdout            结果 PNG 写 stdout（只与 --stdin 同用；此时 --quiet 强制）
  --help, --version
```

- 进度与人类可读信息写 **stderr**；stdout 只输出结果路径（默认，每行一个）或 JSON（--json）或 PNG 字节（--stdout）。
- 退出码：0 全部成功；1 部分/全部处理失败（stderr 写明每张图的错误）；2 参数错误；3 推理引擎不可用（如 runtime/ 缺 DLL、模型文件缺失）。
- 与 `denoise.py` 对齐的行为：输出命名 `<stem>_<method>.png`（默认目录）/ 自定义 out 时同名；`--all` 命名集合与 sheet 布局 4x2 原尺寸并排、带标签，同 `sheet()`；alpha 与 ICC 保留；不支持 stdin 的多图。
- 无控制台窗口需求：从 GUI 调用时用 CreateProcess + CREATE_NO_WINDOW 亦可（不弹窗）。
- 实例互斥：CLI 不参与 GUI 的单实例互斥。
- GPU 名称/DirectML 版本在 --json 的 "device" 字段（"dml:RTX 5060 Ti" / "cpu"）。

## 管线示例

```
anime-denoise out1.png out2.png --json | jq .output
type a.png | anime-denoise - --stdout > a_clean.png
for %f in (*.png) do anime-denoise "%f" --out clean
```

## POSIX / Linux 构建

同一 CLI 也构建于 Linux x86_64（Ubuntu 24.04 验证），产物
`build-posix/anime-denoise`，无 CMake、无 GUI 依赖：

```bash
make -f tools/Makefile.posix -j$(nproc)
```

运行前提：`<exe>/runtime/libonnxruntime.so`（dlopen；找不到时退回动态
链接器默认搜索路径）与 `<exe>/models/`。`servers/bren9np/anime-denoise/
setup.sh` 一键完成 ONNX Runtime 下载、构建与目录摆放。

与 Windows 版的行为差异（仅平台使然，输出语义一致）：

- **仅 CPU**：`--device gpu|auto` 打印一行说明后按 CPU 运行；
  `--json` 的 `device` 恒为 `"cpu"`。
- **图像 I/O 走 stb**（`src/posix/img_posix.c`）：PNG/JPEG/WebP 等解码
  RGBA，PNG/JPEG 编码；PNG iCCP、JPEG APP2 ICC、JPEG EXIF 方向、alpha、
  16-bit 输入（按 `v/257+0.5` 缩放）全部保留——与 WIC 路径逐项对齐。
- **sheet 标签**用 stb_truetype 渲染 `assets/fonts/NotoSansSC-Medium.ttf`；
  字体缺失时留空标签，不报错。
- **路径**为 UTF-8 `char`（Windows 为 `wchar_t`）；`--models` 默认
  `$XDG_DATA_HOME/anime-denoise/models` 或 `~/.local/share/anime-denoise/models`。
- **下载** shell 到 `curl`（`src/posix/download_curl.c`），走
  `http_proxy`/`https_proxy` 环境变量；zip 成员解压（stored + raw
  DEFLATE）与 SHA-256 校验语义同 Windows。
- **剪贴板**无 POSIX 对应物，CLI 本来就不用。
- 引擎侧差异收敛在 `src/plat.h`（`src/posix/plat_posix.c` 实现）：
  `dlopen` 取代 `LoadLibraryW`，pthread 取代 `CreateThread`，
  `/proc/self/exe` 取代 `GetModuleFileNameW`，GCC 原子取代 Interlocked。
