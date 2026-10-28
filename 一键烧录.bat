@echo off
rem ============================================================
rem  一键烧录：AI Passport 农场遥控器固件
rem  自动完成：下载 arduino-cli -> 装 ESP32 内核和库 -> 编译 -> 刷入
rem  无需手动装 Arduino IDE。双击本文件即可。
rem ============================================================
chcp 65001 >nul
setlocal EnableDelayedExpansion
cd /d "%~dp0"

set ACLI=tools\arduino-cli.exe
set SKETCH=firmware\aipassport_farm\aipassport_farm.ino

echo.
echo ================ AI Passport 一键烧录 ================
echo.

rem ---- 1. arduino-cli ----
if not exist "%ACLI%" (
  echo [1/5] 下载 arduino-cli ...
  powershell -NoProfile -Command "$ProgressPreference='SilentlyContinue'; Invoke-WebRequest -Uri 'https://downloads.arduino.cc/arduino-cli/arduino-cli_latest_Windows_64bit.zip' -OutFile 'tools\arduino-cli.zip'; Expand-Archive -Path 'tools\arduino-cli.zip' -DestinationPath 'tools' -Force"
  if not exist "%ACLI%" (
    echo [错误] arduino-cli 下载失败。请检查网络后重试。
    pause
    exit /b 1
  )
) else (
  echo [1/5] arduino-cli 已就绪
)

rem ---- 2. ESP32 开发板内核 ----
echo [2/5] 检查 ESP32 开发板支持 ...
"%ACLI%" config init --overwrite >nul 2>&1
"%ACLI%" config add-board-manager https://espressif.com/package_esp32_index.json >nul 2>&1
"%ACLI%" core update-index >nul 2>&1
"%ACLI%" core list | findstr /i "esp32:esp32" >nul 2>&1
if errorlevel 1 (
  echo       安装 esp32:esp32 内核（约 300MB，首次需要几分钟）...
  "%ACLI%" core install esp32:esp32
  if errorlevel 1 (
    echo [错误] ESP32 内核安装失败。请检查网络后重试。
    pause
    exit /b 1
  )
) else (
  echo       esp32:esp32 内核已安装
)

rem ---- 3. 依赖库 + 屏幕引脚配置 ----
echo [3/5] 安装依赖库 ...
"%ACLI%" lib install "TFT_eSPI" >nul 2>&1
"%ACLI%" lib install "ArduinoJson" >nul 2>&1
rem  User_Setup.h 复制进 TFT_eSPI 库目录（屏幕引脚配置，不复制屏幕不亮）
set LIBDIR=
for /f "delims=" %%i in ('"%ACLI%" config get directories.user 2^>nul') do set USERDIR=%%i
if defined USERDIR (
  set LIBDIR=%USERDIR%\libraries
) else (
  set LIBDIR=%USERPROFILE%\Documents\Arduino\libraries
)
if exist "%LIBDIR%\TFT_eSPI" (
  copy /y "firmware\aipassport_farm\User_Setup.h" "%LIBDIR%\TFT_eSPI\User_Setup.h" >nul
  echo       User_Setup.h 已复制到 TFT_eSPI 库目录
) else (
  echo [警告] 没找到 TFT_eSPI 库目录，屏幕可能不亮。
  echo        请手动把 firmware\aipassport_farm\User_Setup.h
  echo        复制到 Documents\Arduino\libraries\TFT_eSPI\User_Setup.h
)
rem  ★关键补丁：TFT_eSPI 2.5.43 在 ESP32-C3 上 SPI_PORT 用错（SPI2_HOST=1），
rem     导致寄存器地址算成 0x10 直接崩（Guru Meditation: Store access fault）
if exist "%LIBDIR%\TFT_eSPI\Processors\TFT_eSPI_ESP32_C3.h" (
  findstr /c:"#define SPI_PORT 2" "%LIBDIR%\TFT_eSPI\Processors\TFT_eSPI_ESP32_C3.h" >nul 2>&1
  if errorlevel 1 (
    echo       应用 C3 SPI_PORT 补丁 ...
    if not exist "%LIBDIR%\TFT_eSPI\Processors\TFT_eSPI_ESP32_C3.h.bak" (
      copy /y "%LIBDIR%\TFT_eSPI\Processors\TFT_eSPI_ESP32_C3.h" "%LIBDIR%\TFT_eSPI\Processors\TFT_eSPI_ESP32_C3.h.bak" >nul
    )
    powershell -NoProfile -Command "$p='%LIBDIR%\TFT_eSPI\Processors\TFT_eSPI_ESP32_C3.h'; (Get-Content $p -Raw -Encoding UTF8) -replace '#define SPI_PORT SPI2_HOST','#define SPI_PORT 2' | Set-Content $p -Encoding UTF8"
    echo       C3 SPI_PORT 补丁已应用
  ) else (
    echo       C3 SPI_PORT 补丁已存在
  )
)

rem ---- 4. 选择刷机方式 ----
echo [4/5] 选择刷机方式 ...
echo.
echo   1) USB 数据线（首次必须走这条）
echo   2) 无线 OTA（工牌在 WiFi 上、且已刷过含 OTA 的固件）
echo.
set MODE=1
set /p MODE= 请输入 1 或 2 后回车 [默认 1]:
if "!MODE!"=="2" goto OTA

rem ---- 4a. USB：查找串口 ----
echo [4/5] 查找 AI Passport 串口 ...
set PORT=
rem    先问 arduino-cli
for /f "tokens=1" %%p in ('"%ACLI%" board list --format text 2^>nul ^| findstr /i "serial"') do if not defined PORT set PORT=%%p
rem    再按 Espressif USB 设备 (VID_303A) 找
if "!PORT!"=="" (
  for /f "tokens=*" %%p in ('powershell -NoProfile -Command "((Get-CimInstance Win32_PnPEntity ^| Where-Object { $_.DeviceID -like 'USB\VID_303A*' -and $_.Name -match '[(]COM[0-9]+[)]' } ^| ForEach-Object { if ($_.Name -match '[(]COM[0-9]+[)]') { $matches[0].Trim('(',')') } }) -join ' '"') do set PORT=%%p
)
if "!PORT!"=="" (
  echo.
  echo [提示] 没自动找到串口。请确认：
  echo        - AI Passport 已用 USB 数据线连上电脑
  echo        - 设备管理器里能看到 "USB 串行设备 ^(COMxx^)"
  echo.
  set /p PORT= 请手动输入串口号（例如 COM16）:
)
if "!PORT!"=="" (
  echo [错误] 没有指定串口，无法烧录。
  pause
  exit /b 1
)
echo       使用串口: !PORT!

rem ---- 5. 编译 + 烧录 ----
echo [5/5] 编译（首次约 2-3 分钟）...
"%ACLI%" compile --fqbn esp32:esp32:esp32c3:CDCOnBoot=cdc,FlashSize=8M,PartitionScheme=default_8MB,CPUFreq=160 --output-dir tools\build "%SKETCH%"
if errorlevel 1 (
  echo       换用旧内核参数重试...
  "%ACLI%" compile --fqbn esp32:esp32:esp32c3:FlashSize=8M,PartitionScheme=default_8MB --output-dir tools\build "%SKETCH%"
)
if errorlevel 1 (
  echo [错误] 编译失败，请把上面的报错发给开发者。
  pause
  exit /b 1
)

echo       烧录中 ...
rem 用 esptool 直接烧：AI Passport 的 USB Serial/JTAG 必须用 --before usb_reset，
rem 且 stub 在该板上会读不到 flash，必须 --no-stub
set ESPTOOL=%LOCALAPPDATA%\Arduino15\packages\esp32\tools\esptool_py\4.5.1\esptool.exe
if not exist "%ESPTOOL%" (
  for /f "delims=" %%e in ('dir /b /s "%LOCALAPPDATA%\Arduino15\packages\esp32\tools\esptool_py\esptool.exe" 2^>nul') do set ESPTOOL=%%e
)
set BOOTAPP=%LOCALAPPDATA%\Arduino15\packages\esp32\hardware\esp32\2.0.17\tools\partitions\boot_app0.bin
"%ESPTOOL%" --chip esp32c3 --port !PORT! --baud 115200 --before usb_reset --after hard_reset --no-stub write_flash -z 0x0 tools\build\aipassport_farm.ino.bootloader.bin 0x8000 tools\build\aipassport_farm.ino.partitions.bin 0xe000 "%BOOTAPP%" 0x10000 tools\build\aipassport_farm.ino.bin
if errorlevel 1 (
  echo.
  echo [错误] 编译或烧录失败。常见原因：
  echo   1. 串口被其他程序占用（关掉其他串口工具再试）
  echo   2. USB 线是充电线不是数据线
  echo   3. 板子需要手动进下载模式：拔掉 USB，按住机身 OK 键不放，
  echo      插上 USB，看到 "Connecting..." 后再松手
  echo.
  pause
  exit /b 1
)

echo.
echo ================ 烧录成功！===============
echo 接下来：
echo   1. 双击「启动桥接.bat」（图标在右下角，不弹黑框）
echo   2. 确认拓竹农场管家客户端在运行
echo   3. 工牌上应该显示农场状态了
echo.
pause
goto :eof

:OTA
echo.
echo 先确认工牌在 WiFi 上 ...
set DEVIP=
for /f "tokens=*" %%a in ('powershell -NoProfile -Command "$r=(Get-NetTCPConnection -State Established -ErrorAction SilentlyContinue ^| Where-Object { $_.RemotePort -eq 8897 }); if($r){ $r[0].RemoteAddress }"') do set DEVIP=%%a
if "!DEVIP!"=="" (
  for /f "tokens=*" %%a in ('powershell -NoProfile -Command "(Get-Content '.tmp\bridge.log' -Tail 60 -ErrorAction SilentlyContinue ^| Select-String -Pattern 'tcp.+.(\d+\.\d+\.\d+\.\d+)' -AllMatches ^| ForEach-Object { $_.Matches } ^| ForEach-Object { $_.Groups[1].Value } ^| Select-Object -Last 1)"') do set DEVIP=%%a
)
if "!DEVIP!"=="" (
  echo       （没查到工牌 IP，没关系 —— 桥接会自己找出在线的工牌）
) else (
  echo       工牌 IP: !DEVIP!
)
echo.
echo       提醒：无线刷机要靠桥接服务转发，请确认右下角有桥接图标。
echo       没有的话先双击「启动桥接.bat」
echo.
echo       编译 ...
"%ACLI%" compile --fqbn esp32:esp32:esp32c3:CDCOnBoot=cdc,FlashSize=8M,PartitionScheme=default_8MB,CPUFreq=160 --output-dir tools\build "%SKETCH%"
if errorlevel 1 (
  echo [错误] 编译失败。
  pause
  exit /b 1
)
echo       无线刷入中（工牌屏幕会显示"正在无线升级固件"）...
node tools\ota_wifi.js
if errorlevel 1 (
  echo.
  echo [错误] 无线刷入失败。上面已经写了原因，常见几条：
  echo   1. 桥接服务没在跑 —— 先双击「启动桥接.bat」
  echo   2. 工牌当前固件不含新版 OTA —— 用 USB 刷一次就有无线能力了
  echo   3. 工牌掉线 —— 看桥接日志里有没有 [tcp] 已连上工牌
  echo.
  pause
  exit /b 1
)
echo.
echo ================ 无线刷入成功！===============
echo 工牌会自动重启并连回桥接。
echo.
pause
