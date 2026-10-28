
$cfg = Get-Content 'C:\Users\zzz\AppData\Local\Temp\lnk_startup.json' -Raw -Encoding UTF8 | ConvertFrom-Json
$ws = New-Object -ComObject WScript.Shell
$startup = [Environment]::GetFolderPath('Startup')
Get-ChildItem $startup -Filter '_tmp*.lnk' | Remove-Item -Force
$lnk = Join-Path $startup '_tmp_bridge_startup.lnk'
if (Test-Path $lnk) { Remove-Item $lnk -Force }
$s = $ws.CreateShortcut($lnk)
$s.TargetPath = $cfg.bat
$s.WorkingDirectory = $cfg.dir
$s.IconLocation = $cfg.ico + ',0'
$s.Description = $cfg.desc
$s.WindowStyle = 7
$s.Save()
$v = $ws.CreateShortcut($lnk)
$out = @()
$out += ("Target: " + $v.TargetPath)
$out += ("Style: " + $v.WindowStyle)
[IO.File]::WriteAllLines('C:\Users\zzz\AppData\Local\Temp\startup_verify.txt', $out, [Text.Encoding]::UTF8)
