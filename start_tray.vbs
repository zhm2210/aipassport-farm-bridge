' 农场桥接 - 托盘模式启动器（双击无任何窗口弹出）
Set fso = CreateObject("Scripting.FileSystemObject")
Set sh = CreateObject("WScript.Shell")
root = fso.GetParentFolderName(WScript.ScriptFullName)
cmd = "powershell.exe -WindowStyle Hidden -NoProfile -ExecutionPolicy Bypass -File """ & root & "\server\tray.ps1"""
sh.Run cmd, 0, False
