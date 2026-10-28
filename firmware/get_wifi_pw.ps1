chcp 65001 | Out-Null
$out = cmd /c "chcp 65001 > nul & netsh wlan show profile name=""wesley"" key=clear" 2>&1 | Out-String
# 只提取 Key Content 那一行，单独存文件（避免中文标签乱码）
$lines = $out -split "`r?`n"
$pw = ''
foreach ($l in $lines) {
  if ($l -match '^\s*Key Content\s*:\s*(.+?)\s*$') { $pw = $matches[1] }
}
[IO.File]::WriteAllText('C:\Users\zzz\AppData\Local\Temp\wifi_pw.txt', $pw, [Text.Encoding]::UTF8)
Write-Output ("pwlen=" + $pw.Length)
