# ttkConverter

ttkConverter 是一款使用 C++17 和原生 Windows 界面开发的离线转换工具。视频、音频、图片和 PDF 转 Word 共用一个任务队列，安装包自带转换引擎和简体中文、英文 OCR 模型。

![ttkConverter 欢迎页](docs/welcome.png)

## 选择安装包

| 对应系统 | 安装包 |
| --- | --- |
| Windows 10 或 Windows 11，64 位 | `ttkConverter-1.3.0-Win10-11-x64-Setup.exe` |
| Windows 7 SP1，64 位 | `ttkConverter-1.3.0-Win7-SP1-x64-Setup.exe` |
| Windows 7 SP1，32 位 | `ttkConverter-1.3.0-Win7-SP1-x86-Setup.exe` |

从 [GitHub Releases](https://github.com/p-arrot/ttkConverter/releases/latest) 下载对应安装包。安装包为当前用户安装，并自带 FFmpeg 和 OCR 引擎。Windows 7 可在「控制面板 → 系统」查看位数，用 `winver` 核对 SP1（内部版本 7601）。可用 `certutil -hashfile "安装包路径" SHA256` 对照发布页的校验值。

首次安装的位置为 `%LOCALAPPDATA%\Programs\ttkConverter`，开始菜单显示「ttkConverter」。检测到 1.1.x 或 1.2.x 时，安装器显示版本和目录，并在原目录升级。检测到旧路径中的 1.0.0 时，可以在新版安装成功后移除旧程序，也可以保留旧版。应用设置和转换结果保留。

## 转换文件

- 点击「添加文件」、按 `Ctrl+O` 或拖入文件，程序会分类并推荐输出。选中视频可在「源文件」看到原封装、编码、像素尺寸和画幅，转换前可查看预计输出，完成后可查看成品的实际尺寸和画幅。
- 视频可直接选择「原始」、144p、240p、360p、480p、540p、720p、1080p、1440p 或 4K，也可以在高级设置里指定像素宽度和高度。16:9 时，1080p 对应 1920×1080，1440p 对应 2560×1440，4K 对应 3840×2160。预设按原画幅缩放，例如 4:3 视频选择 1080p 时输出 1440×1080。
- 质量与分辨率分别选择。质量预设包含快速、高画质、平衡、较小体积、低画质和极低画质。
- 选中带音轨的视频，点「提取音频」即可切到音频输出。默认 MP3，可改选 WAV、FLAC、M4A 等。点「转回视频」可恢复视频输出。
- 图片可选原始尺寸、长边 320 至 3840 像素或自定义宽高。有损格式另有画质档位。ICO 提供 16 至 256 像素的方形尺寸预设，有需要时用透明区域填充。
- 音频按输出格式提供采样率。支持有损码率的格式可选自动或 32 至 320 kb/s 的常用档位，可用码率随编码和采样率更新。选择自动码率时由音质档位决定压缩程度。
- PDF 转 Word 默认「可编辑文字」。对于各页均有文字层的 PDF，OpenDoc 离线保留页面和图文位置，把正文保存为可编辑文本框。表格线和流程图按原位置重建。表格外观可保留，单元格不一定会生成 Word 原生表格。
- 含扫描页的 PDF 会提取已有文字层，并用 Tesseract 识别扫描页。OCR 提供 200、300、400 DPI，改进双栏阅读顺序、标题和简单表格。也可在高级设置中强制整份 PDF 进行 OCR。OpenDoc 排版需要支持 SSE4.1 的处理器，运行时不可用时会使用基础排版。
- 「保留原版式」以整页图片保留颜色、位置和横竖方向。可在 Word 中编辑整页图片，清晰度可选 150、200、300 或 400 DPI。
- 可以随时添加文件、更改未开始的任务和保存位置，再点击「开始转换」。默认结果写到源文件旁的「ttkConverter 输出」文件夹。同名输出自动加序号。通过「任务操作」可以打开结果、查看失败详情或重试。

欢迎图、左上角头像和程序 ICO 均以用户提供的橘猫照片为参考，用内置 ImageGen 生成日系赛璐璐插画。PNG 源文件和完整提示词在 `assets/`。应用标题用内嵌的 ttk Rounded（基于 Nunito），中文界面使用系统字体。

### 支持的输出

| 类别 | 输出格式 |
| --- | --- |
| 视频 | MP4、MKV、MOV、WebM、AVI、WMV、MPG、TS、FLV。MP4 与 MKV 提供 H.264、H.265、AV1，MOV 提供 H.264、H.265，WebM 提供 VP8、VP9、AV1。 |
| 音频 | MP3、AAC、M4A（AAC 或 ALAC）、WAV、FLAC、AIFF、OGG、Opus、AC-3。也可从视频提取音频。 |
| 图片 | JPEG、PNG、WebP、BMP、TIFF、TGA、ICO、AVIF、GIF、APNG。可读取常见静态 HEIC。需要保留动画时可选 GIF 或 APNG。 |
| 办公 | PDF 转 Word（DOCX），支持保留原版式和可编辑文字两种模式。 |

H.264 预设适合常见设备。H.265 和 AV1 可选择 CPU 编码，在旧电脑上可能耗时较长。现代版的硬件编码选项依赖显卡、驱动和所选编码器，无法启用时会尝试软件编码。无损换封装要求源音视频流与目标容器兼容，保留原尺寸和码率。

## 构建与测试

在 Windows 构建机上准备 PowerShell 7、CMake 3.24 或更新版本、Ninja、Git 和 MSYS2 UCRT64。构建媒体引擎时，MSYS2 的 `PATH` 需要能找到 `make`、`pkgconf`、`cmake` 和 `ninja`。构建脚本按 SHA-256 校验固定的依赖归档，并准备 LLVM MinGW 和 Inno Setup。

PowerShell 与 MSYS2 设置同一个 `TTK_BUILD_ROOT`。默认使用 `%LOCALAPPDATA%\ttkconverter-build`。如果默认路径含空格或非 ASCII 字符，为第三方编译选择一个简短的英文工作目录。

1. 在 PowerShell 中运行 `./tools/prepare-deps.ps1 -InstallInno -StagePdf`，下载依赖并从固定源码准备 32 位和 64 位 PDF/OCR 引擎。
2. 在 MSYS2 Bash 中运行 `bash tools/build-media.sh x86` 和 `bash tools/build-media.sh x64`，然后运行 `bash tools/build-media.sh modern`。现代媒体引擎复用 x64 编解码库。
3. 回到 PowerShell，运行 `./tools/prepare-opendoc.ps1` 准备离线版式引擎，再运行 `./tools/build-app.ps1 -Edition Win10-11-x64 -Package`。`-Edition` 也可选 `Win7-SP1-x64` 或 `Win7-SP1-x86`，安装包写入 `release/`。如果固定版本的 OpenDoc 部署文件已更新，可从发布页的第三方源码压缩包取出 `downloads/opendoc/` 放在构建目录下。
4. 对包含 `engines/` 的暂存或已安装程序运行 `python tools/smoke-test.py --app <程序路径> --all-formats --fixtures <样本目录>`。样本可从发布页下载。自行生成样本时需要 Pillow、ReportLab、简体中文字体和支持 `lavfi` 的开发用 FFmpeg，通过 `--ffmpeg` 指定它。

## 交付验收清单

发布页的验收记录列出了 Win11 上通过的安装和转换用例。Win7 兼容包仍需在对应位数的 Win7 SP1 实机上核对。

- [ ] 分别在 Windows 10 或 11 x64、Windows 7 SP1 x64、Windows 7 SP1 x86 的目标电脑上校验对应安装包并安装。用普通用户账户打开程序，验证中文显示、桌面拖放、键盘操作和小窗口里主按钮的可见性。
- [ ] 断网后一次加入带中文和空格的 MP4、PNG 与中英扫描 PDF。验证队列分类、源视频参数、中文高级设置、进度和「打开结果」。
- [ ] 从同一段短视频输出 H.264、H.265、AV1，并用安装包内的 FFmpeg 或支持相应编码的播放器完整解码。测试 1080p、4K、竖屏、极低画质和同名输出。
- [ ] 提取 64 kb/s MP3 并核对采样率。调整透明 PNG 长边并输出方形 ICO。用文本、扫描和混合 PDF 验证版式模式、可编辑文字和表格，以及 200 至 400 DPI 的 OCR 设置。
- [ ] 转换长视频时停止批次。确认当次未生成半成品，其他任务仍可继续。再测试失败详情、重试、关闭、卸载和重新安装。
- [ ] 用新版安装包识别并升级已有的 1.1.x，核对旧安装目录、设置及转换文件。迁移测试可运行 `tools/test-upgrade.ps1` 并传入新版、1.1.1 和 1.0.0 的 Windows 10/11 x64 安装包。

## 许可证

应用源码和项目自有素材使用 [MIT 许可证](LICENSE)。内嵌的 ttk Rounded 字体使用 [SIL OFL 1.1](assets/ttk-rounded-OFL.txt)。随包引擎按照各自的许可证分发，许可索引见 [第三方组件](installer/THIRD-PARTY.txt)，对应源码和构建脚本可在同一发布页获取。
