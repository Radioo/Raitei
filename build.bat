@echo off
setlocal
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set "VS_DIR=%%i"
call "%VS_DIR%\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64 -no_logo || exit /b 1
cmake --preset release || exit /b 1
cmake --build --preset release || exit /b 1
