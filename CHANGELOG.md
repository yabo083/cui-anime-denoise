# Changelog

本项目版本与 GitHub Release 一一对应；GUI 内自动更新读取 Release 资产
`update.json`（`releases/latest/download/` 始终指向最新版）。

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
