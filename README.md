# TrayAudio

TrayAudio 是一个面向 Windows 的托盘音频工具。它使用原生 Win32 和 Windows Core Audio，集中管理默认设备、系统音量和各应用音量。

## 下载

从 [GitHub Releases 下载 TrayAudio.exe](https://github.com/SaltyF111sh/TrayAudio/releases/latest/download/TrayAudio.exe)，双击即可运行。程序为 Windows x64 单文件版，支持离线使用，无需安装字体或额外运行库。已在 Windows 10 22H2 x64 上验证。

## 使用

右键单击托盘图标，选择「关于」，可在程序内查看使用说明。

- 左键单击托盘图标打开「混合」页，管理默认输出、系统音量和应用音量。点击右上角「输出」展开设备。
- 「语音」页管理默认输入、麦克风电平和通信设备。点击右上角「输入」展开设备。输入和输出都可以点设备名切换，点右端图钉固定。固定列表会在重启后保留。
- 点击「开始测试」可将当前麦克风的声音实时播放到默认输出，播放音量随系统输出音量。点击「停止测试」、离开语音页、关闭面板或切换设备都会结束测试。
- 右键单击托盘图标可快速切换设备、调整配色和透明度、控制登录自启。透明度范围为 0% 到 80%，每档相差 10%。
- 拖动标题栏调整位置，拖动窗口边缘调整大小。程序会保存面板位置、尺寸、配色、透明度及应用音量和静音设置。

首次打开时，程序会为当前用户开启登录自启，可在托盘菜单关闭。

## 构建

需要 Visual Studio 2022 或 Build Tools 的 C++ 桌面开发工具、Windows SDK（已验证 10.0.26100）和 Python 3。在项目根目录运行 `build.cmd`，输出位于 `build\TrayAudio.exe`。

首次构建会从小米官网下载 MiSans Regular 和官方许可协议，并原样嵌入 EXE。MiSans 可免费商用，小米允许在软件中嵌入使用。详见[小米官方授权说明](https://hyperos.mi.com/font/download)。
