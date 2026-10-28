$cfg = Get-Content 'C:\Users\zzz\AppData\Local\Temp\lnk_cfg.json' -Raw -Encoding UTF8 | ConvertFrom-Json
$ws = New-Object -ComObject WScript.Shell
$desktop = [Environment]::GetFolderPath('Desktop')
$lnk = Join-Path $desktop '_tmp_farm_bridge.lnk'
if (Test-Path $lnk) { Remove-Item $lnk -Force }
$s = $ws.CreateShortcut($lnk)
$s.TargetPath = $cfg.bat
$s.WorkingDirectory = $cfg.dir
$s.IconLocation = $cfg.ico + ',0'
$s.Description = $cfg.desc
$s.WindowStyle = 1
$s.Save()
$v = $ws.CreateShortcut($lnk)
$out = @()
$out += ("Target: " + $v.TargetPath)
$out += ("WorkDir: " + $v.WorkingDirectory)
$out += ("Icon: " + $v.IconLocation)
$out += ("Desc: " + $v.Description)
[IO.File]::WriteAllLines('C:\Users\zzz\AppData\Local\Temp\lnk_verify.txt', $out, [Text.Encoding]::UTF8)
