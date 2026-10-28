// Point the startup shortcut at the silent autostart bat
const fs = require('fs');
const { execFileSync } = require('child_process');

const cfg = {
  bat: 'C:\\Users\\zzz\\WorkBuddy\\农场\\start-bridge-autostart.bat',
  dir: 'C:\\Users\\zzz\\WorkBuddy\\农场',
  ico: 'C:\\Users\\zzz\\WorkBuddy\\农场\\app.ico',
  desc: '开机自动运行农场桥接（静默后台，不打开浏览器）',
};
fs.writeFileSync('C:/Users/zzz/AppData/Local/Temp/lnk_startup.json', JSON.stringify(cfg, null, 1));

const ps = `
$cfg = Get-Content 'C:\\Users\\zzz\\AppData\\Local\\Temp\\lnk_startup.json' -Raw -Encoding UTF8 | ConvertFrom-Json
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
[IO.File]::WriteAllLines('C:\\Users\\zzz\\AppData\\Local\\Temp\\startup_verify.txt', $out, [Text.Encoding]::UTF8)
`;
fs.writeFileSync('C:/Users/zzz/WorkBuddy/农场/firmware/make_startup.ps1', ps, 'utf8');
execFileSync('powershell.exe', ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', 'C:\\Users\\zzz\\WorkBuddy\\农场\\firmware\\make_startup.ps1'], { encoding: 'utf8' });
console.log(fs.readFileSync('C:/Users/zzz/AppData/Local/Temp/startup_verify.txt', 'utf8'));
