@echo off
setlocal
pushd "%~dp0"
set "vswhere=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not defined VCToolsInstallDir (
    for /f "usebackq delims=" %%I in (`"%vswhere%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "vs=%%I"
    if not defined vs (
        echo Visual Studio Build Tools with C++ and the Windows SDK are required.
        popd
        exit /b 1
    )
)
if not defined VCToolsInstallDir (
    goto setup
)
:compile
if not exist build mkdir build
if not exist build\MiSans-Regular.ttf (
    python fetch-misans.py build\MiSans-Regular.ttf
    if errorlevel 1 goto failed
)
rc /nologo /fo build\TrayAudio.res res\TrayAudio.rc
if errorlevel 1 goto failed
cl /nologo /std:c++20 /utf-8 /EHsc /O2 /MT /W4 /DUNICODE /D_UNICODE /DNOMINMAX /DWINVER=0x0A00 /D_WIN32_WINNT=0x0A00 /Fo:build\ /Fe:build\TrayAudio.exe Panel.cpp Audio.cpp build\TrayAudio.res /link /SUBSYSTEM:WINDOWS /MANIFEST:NO user32.lib gdi32.lib shell32.lib ole32.lib advapi32.lib dwmapi.lib shlwapi.lib version.lib uuid.lib coremessaging.lib windowsapp.lib
if errorlevel 1 goto failed
popd
exit /b 0

:setup
rem Quoted PATH entries break the toolchain's environment detection.
set "PATH=%PATH:"=%"
call "%vs%\Common7\Tools\VsDevCmd.bat" -no_logo -arch=amd64 -host_arch=amd64
if errorlevel 1 goto failed
goto compile

:failed
popd
exit /b 1
