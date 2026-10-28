# 在桌面创建「农场桥接」启动快捷方式（指向 启动桥接.bat）
$ErrorActionPreference = 'Stop'
$ws   = New-Object -ComObject WScript.Shell
$root = 'C:\Users\zzz\WorkBuddy\农场'
$bat  = Join-Path $root '启动桥接.bat'
$ico  = Join-Path $root 'app.ico'
$desktop = [Environment]::GetFolderPath('Desktop')
$lnk  = Join-Path $desktop '农场桥接.lnk'

$sc = $ws.CreateShortcut($lnk)
$sc.TargetPath      = $bat
$sc.WorkingDirectory = $root
$sc.Arguments       = ''
$sc.WindowStyle     = 7          # 7 = 最小化运行（bat 一闪即退，干凈）
$sc.Description     = '农场桥接（托盘图标：状态/重启/开机自启）'
if (Test-Path $ico) { $sc.IconLocation = $ico }
$sc.Save()
Write-Host "已创建桌面快捷方式: $lnk"
