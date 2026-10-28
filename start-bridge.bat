@echo off
rem 农场桥接 - 托盘模式：不弹黑框，图标在右下角（绿=正常/橙=工牌离线/灰=桥接未起）
rem 旧版是 node launch.js（弹黑框+开浏览器，无图标）；改走托盘，双击即有右下角图标
start "" wscript.exe "%~dp0start_tray.vbs"
