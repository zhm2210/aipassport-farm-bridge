chcp 65001 | Out-Null
$env:PYTHONIOENCODING = 'utf-8'
$out = cmd /c "chcp 65001 > nul & netsh wlan show profile name=""wesley"" key=clear" 2>&1 | Out-String
[IO.File]::WriteAllText('C:\Users\zzz\AppData\Local\Temp\wifi_wesley.txt', $out, [Text.Encoding]::UTF8)
Write-Output ("len=" + $out.Length)
