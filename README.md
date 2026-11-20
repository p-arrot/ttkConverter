# ttkConverter

ttkConverter 是一款使用 C++17 和原生 Windows 界面开发的离线转换工具。视频、音频、图片和 PDF 转 Word 共用一个任务队列，安装包自带转换引擎和简体中文、英文 OCR 模型。

![ttkConverter 欢迎页](docs/welcome.png)

## 选择安装包

| 对应系统 | 安装包 |
| --- | --- |
| Windows 10 或 Windows 11，64 位 | `ttkConverter-1.1.1-Win10-11-x64-Setup.exe` |
| Windows 7 SP1，64 位 | `ttkConverter-1.1.1-Win7-SP1-x64-Setup.exe` |
| Windows 7 SP1，32 位 | `ttkConverter-1.1.1-Win7-SP1-x86-Setup.exe` |

从 [GitHub Releases](https://github.com/p-arrot/ttkConverter/releases/latest) 下载对应安装包。安装包为当前用户安装，并自带 FFmpeg 和 OCR 引擎。Windows 7 可在「控制面板 → 系统」查看位数，用 `winver` 核对 SP1（内部版本 7601）。可用 `certutil -hashfile "安装包路径" SHA256` 对照发布页的校验值。

新版首次安装位置为 `%LOCALAPPDATA%\Programs\ttkConverter`，开始菜单显示「ttkConverter」。如果同时保留了 1.0.0，带系统版本后缀的快捷方式仍指向旧版。

## 转换文件

1. 在橘猫欢迎页点击「添加文件」，或直接把文件拖进窗口。也可按 `Ctrl+O`。程序会分类并推荐输出，每个文件的格式和状态在加入队列后才显示。
2. 选中任务可改用常用格式，在下拉菜单中点「显示全部格式」即可展开列表。选中多个同类任务可以一起设置。若需要分辨率、码率、声道、线程、无损换封装或强制 OCR，打开「高级设置」。数字参数填 0 使用自动设置。
3. 在底部查看保存位置，再点击「开始转换」。默认结果写到各源文件旁的「ttkConverter 输出」文件夹，同名输出自动加序号。
4. 选中任务后，点「任务操作」可打开结果、查看失败详情或重试。正在运行的批次可以停止。

欢迎图、左上角头像和程序 ICO 均以用户提供的橘猫照片为参考，用内置 ImageGen 生成日系赛璐璐插画。PNG 源文件和完整提示词在 `assets/`。应用标题用内嵌的 ttk Rounded（基于 Nunito），中文界面使用系统字体。

### 支持的输出

| 类别 | 输出格式 |
| --- | --- |
| 视频 | MP4、MKV、MOV、WebM、AVI、WMV、MPG、TS、FLV。MP4 与 MKV 提供 H.264、H.265、AV1，MOV 提供 H.264、H.265，WebM 提供 VP8、VP9、AV1。 |
| 音频 | MP3、AAC、M4A（AAC 或 ALAC）、WAV、FLAC、AIFF、OGG、Opus、AC-3。也可从视频提取音频。 |
| 图片 | JPEG、PNG、WebP、BMP、TIFF、TGA、ICO、AVIF、GIF、APNG。可读取常见静态 HEIC。需要保留动画时可选 GIF 或 APNG。 |
| 办公 | PDF 转可编辑 Word（DOCX）。直接提取有文字层的页面，自动对扫描页运行简体中文和英文 OCR，可重建部分简单表格。复杂表格、多栏、罕见字体和低清扫描件需对照原 PDF 校对。 |

H.264 预设适合常见设备。H.265 和 AV1 可选择 CPU 编码，在旧电脑上可能耗时较长。现代版的硬件编码选项依赖显卡、驱动和所选编码器，无法启用时会尝试软件编码。无损换封装要求源音视频流与目标容器兼容，保留原尺寸和码率。

## 构建与测试

在 Windows 构建机上准备 PowerShell 7、CMake 3.24 或更新版本、Ninja、Git 和 MSYS2 UCRT64。构建媒体引擎时，MSYS2 的 `PATH` 需要能找到 `make`、`pkgconf`、`cmake` 和 `ninja`。构建脚本按 SHA-256 校验固定的依赖归档，并准备 LLVM MinGW 和 Inno Setup。

PowerShell 与 MSYS2 设置同一个 `TTK_BUILD_ROOT`。默认使用 `%LOCALAPPDATA%\ttkconverter-build`。如果默认路径含空格或非 ASCII 字符，为第三方编译选择一个简短的英文工作目录。

1. 在 PowerShell 中运行 `./tools/prepare-deps.ps1 -InstallInno -StagePdf`，下载依赖并从固定源码准备 32 位和 64 位 PDF/OCR 引擎。
2. 在 MSYS2 Bash 中运行 `bash tools/build-media.sh x86` 和 `bash tools/build-media.sh x64`，然后运行 `bash tools/build-media.sh modern`。现代媒体引擎复用 x64 编解码库。
3. 回到 PowerShell，运行 `./tools/build-app.ps1 -Edition Win10-11-x64 -Package`。`-Edition` 也可选 `Win7-SP1-x64` 或 `Win7-SP1-x86`，安装包写入 `release/`。
4. 对包含 `engines/` 的暂存或已安装程序运行 `python tools/smoke-test.py --app <程序路径> --all-formats`。生成测试样本需要 Pillow、ReportLab 和简体中文字体。可以设置 `TTK_FIXTURE_CHINESE_FONT`，或通过 `--fixtures` 使用发布页的验收样本。

## 交付验收清单

发布页的验收记录列出了 Win11 上通过的安装和转换用例。Win7 兼容包仍需在对应位数的 Win7 SP1 实机上核对。

- [ ] 分别在 Windows 10 或 11 x64、Windows 7 SP1 x64、Windows 7 SP1 x86 的目标电脑上校验对应安装包并安装。用普通用户账户打开程序，验证中文显示、桌面拖放、键盘操作和小窗口里主按钮的可见性。
- [ ] 断网后一次加入带中文和空格的 MP4、PNG 与中英扫描 PDF。验证队列分类、进度、转换结果和「打开结果」。
- [ ] 从同一段短视频输出 H.264、H.265、AV1，并用安装包内的 FFmpeg 或支持相应编码的播放器完整解码。测试音频提取、透明图片、动画和同名输出。
- [ ] 分别用文本 PDF、扫描 PDF 和混合 PDF 转成 DOCX。在 Word 中选中并修改文字，核对常见表格的单元格以及中英识别结果。
- [ ] 转换长视频时停止批次。确认当次未生成半成品，其他任务仍可继续。再测试失败详情、重试、关闭、卸载和重新安装。

## 许可证

应用源码和项目自有素材使用 [MIT 许可证](LICENSE)。内嵌的 ttk Rounded 字体使用 [SIL OFL 1.1](assets/ttk-rounded-OFL.txt)。随包引擎按照各自的许可证分发，许可索引见 [第三方组件](installer/THIRD-PARTY.txt)，对应源码和构建脚本可在同一发布页获取。
