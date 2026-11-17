# TrayAudio

TrayAudio 是一个面向 Windows 的托盘音频工具。它使用原生 Win32 和 Windows Core Audio，集中管理默认设备、系统音量和各应用音量。

## 使用

- 左键单击托盘图标打开面板。「混合」页管理默认输出和应用音量。「语音」页管理默认输入、麦克风电平和通信设备。
- 右键单击托盘图标可快速切换设备、调整配色和透明度、控制登录自启。
- 拖动标题栏调整位置，拖动窗口边缘调整大小。程序会保存面板位置、尺寸、配色、透明度及应用音量和静音设置。

双击 `TrayAudio.exe` 即可运行。首次打开时，程序会为当前用户开启登录自启。程序已在 Windows 10 22H2 x64 上验证。

## 构建

需要 Visual Studio 2022 或 Build Tools 的 C++ 桌面开发工具、Windows SDK（已验证 10.0.26100）和 Python 3。在项目根目录运行 `build.cmd`，输出位于 `build\TrayAudio.exe`。

首次构建时，脚本会从小米官网取得 MiSans Regular 并原样嵌入程序。界面使用小米 [MiSans](https://hyperos.mi.com/font/zh/)，使用条款见[小米官方授权说明](https://hyperos.mi.com/font/zh/download/)。
