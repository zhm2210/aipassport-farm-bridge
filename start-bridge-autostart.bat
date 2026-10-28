@echo off
rem autostart: 改走托盘模式（无窗口，图标在右下角；右键可暂停/重启）
start "" wscript.exe "%~dp0start_tray.vbs"
