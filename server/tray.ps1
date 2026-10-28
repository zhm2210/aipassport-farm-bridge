#requires -Version 5.1
param([switch]$NoTray)
<#
  tray.ps1 — 农场桥接的托盘程序（右下角图标 + 右键菜单）
  - 桥接在后台跑，没有黑框控制台，日志写文件
  - 右键图标：第一行就是实时状态 / 打开状态页 / 查看日志 / 开机自启动 / 重启桥接 / 退出
  - 图标颜色一眼看状态：绿=正常  橙=工牌离线  灰=桥接没起来
  - 状态变了才弹一次气泡提醒，平时不打扰
  - 每 10 秒自检一次，桥接挂了会自动拉起来
  - 加 -NoTray 只做桥接管理（不建图标），用于命令行自检排错
#>

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

# 本机地址别走代理：这台机器上代理环境变量会让 Invoke-RestMethod 连不上 127.0.0.1
try { [System.Net.WebRequest]::DefaultWebProxy = New-Object System.Net.WebProxy } catch { }

$Root    = Split-Path -Parent $PSScriptRoot
$LogDir  = Join-Path $Root '.tmp'
if (-not (Test-Path $LogDir)) { New-Item -ItemType Directory -Path $LogDir -Force | Out-Null }
$OutLog  = Join-Path $LogDir 'bridge_out.log'
$ErrLog  = Join-Path $LogDir 'bridge_err.log'
$TrayLog = Join-Path $LogDir 'tray.log'
$IconPath = Join-Path $Root 'app.ico'
$StateURL = 'http://127.0.0.1:8899/state'
$RegPath  = 'HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Run'
$RegName  = 'FarmBridgeTray'

$script:BridgeProc = $null
$script:Paused     = $false          # 手动暂停后置真：暂停期间不再自动拉起桥接
$StartupDir = [Environment]::GetFolderPath('Startup')
$AutoVbs    = Join-Path $StartupDir '拓竹农场桥接.vbs'

# 托盘自己也要留日志：图标没出来 / 桥接没起来时，靠它排错
function Write-TLog([string]$msg) {
  try { Add-Content -Path $TrayLog -Value ('[' + (Get-Date).ToString('HH:mm:ss') + '] ' + $msg) -Encoding UTF8 } catch { }
}
Write-TLog '===== 托盘启动 ====='

# ---------- 桥接进程管理 ----------
function Test-BridgeAlive {
  $c = Get-NetTCPConnection -LocalPort 8899 -State Listen -ErrorAction SilentlyContinue
  return ($null -ne $c)
}

$script:LastTryStart = Get-Date '2000-01-01'
function Start-Bridge {
  if (Test-BridgeAlive) { return $true }
  # 起不来时别每 10 秒疯狂拉进程，至少间隔 30 秒试一次
  if (((Get-Date) - $script:LastTryStart).TotalSeconds -lt 30) { return $false }
  $script:LastTryStart = Get-Date
  # 注意：不能用 Start-Process —— 这台机器上环境变量同时有 HTTPS_PROXY 和 https_proxy，
  # .NET 的哈希表不区分大小写 → 直接抛异常。改用 System.Diagnostics.Process 绕开。
  $node = (Get-Command node.exe -ErrorAction SilentlyContinue | Select-Object -First 1).Source
  if (-not $node) { Write-TLog '找不到 node.exe，桥接起不来'; return $false }
  $launcher = Join-Path $Root 'server\launch.js'
  if (-not (Test-Path $launcher)) { Write-TLog "找不到启动器 $launcher"; return $false }
  try {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $node
    $psi.Arguments = '"' + $launcher + '" --no-browser'
    $psi.WorkingDirectory = $Root
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    # 说明：桥接自己会把日志写到 .tmp/bridge.log，这里就不接控制台输出了
    # （PowerShell 5.1 用 += 订阅 OutputDataReceived 事件会报错，别加）
    $p = [System.Diagnostics.Process]::Start($psi)
    $script:BridgeProc = $p
    Write-TLog "桥接已拉起 node PID=$($p.Id)"
    return $true
  } catch {
    Write-TLog ('拉起桥接失败: ' + $_.Exception.Message)
    return $false
  }
}

function Stop-Bridge {
  Get-NetTCPConnection -LocalPort 8899 -State Listen -ErrorAction SilentlyContinue |
    ForEach-Object { Stop-Process -Id $_.OwningProcess -Force -ErrorAction SilentlyContinue }
}

# ---------- 开机自启 ----------
function Get-AutoStart { return (Test-Path $AutoVbs) }
function Set-AutoStart([bool]$on) {
  if ($on) {
    # 在「启动」文件夹写无窗口 .vbs（UTF-16 + BOM，wscript 原生支持中文路径）
    $lines = @(
      "' FarmBridge autostart - silent, no console window",
      'Set sh = CreateObject("WScript.Shell")',
      ('sh.Run "powershell.exe -WindowStyle Hidden -NoProfile -ExecutionPolicy Bypass -File ""' + $PSCommandPath + '""", 0, False'),
      ''
    )
    Set-Content -Path $AutoVbs -Value ($lines -join "`r`n") -Encoding Unicode -Force
  } else {
    if (Test-Path $AutoVbs) { Remove-Item -LiteralPath $AutoVbs -Force -ErrorAction SilentlyContinue }
  }
  # 清理历史遗留的注册表自启项，统一改由「启动」文件夹的 .vbs 负责
  Remove-ItemProperty -Path $RegPath -Name $RegName -ErrorAction SilentlyContinue
}

if ($NoTray) {
  Write-Host '[tray] 无界面模式（只管桥接，用于自检排错）'
  [void](Start-Bridge)
  Start-Sleep -Seconds 12
  if (Test-BridgeAlive) { Write-Host '[tray] 桥接已启动：8899 在听' }
  else { Write-Host "[tray] 桥接启动失败，日志在 $OutLog" }
  Write-Host ('[tray] 开机自启当前: ' + $(if (Get-AutoStart) { '已开启' } else { '未开启' }))
  Write-Host "[tray] 日志目录: $LogDir"
  exit 0
}

# ---------- 托盘图标 ----------
$notify = New-Object System.Windows.Forms.NotifyIcon
if (Test-Path $IconPath) {
  $notify.Icon = New-Object System.Drawing.Icon($IconPath)   # 启动瞬间先用自带图标
}
$notify.Text = '农场桥接 启动中…'
$notify.Visible = $true

$menu = New-Object System.Windows.Forms.ContextMenu

# 第 1、2 行：实时状态（灰色不可点，纯展示）
$mHead = New-Object System.Windows.Forms.MenuItem -ArgumentList '启动中…'
$mHead.Enabled = $false
$menu.MenuItems.Add($mHead) | Out-Null

$mInfo = New-Object System.Windows.Forms.MenuItem -ArgumentList '  正在获取状态…'
$mInfo.Enabled = $false
$menu.MenuItems.Add($mInfo) | Out-Null

$menu.MenuItems.Add((New-Object System.Windows.Forms.MenuItem -ArgumentList '-')) | Out-Null

$mStatus = New-Object System.Windows.Forms.MenuItem -ArgumentList '打开状态页（浏览器）'
$mStatus.Add_Click({
  Start-Process $StateURL
})
$menu.MenuItems.Add($mStatus) | Out-Null

$mLog = New-Object System.Windows.Forms.MenuItem -ArgumentList '查看运行日志'
$mLog.Add_Click({
  if (Test-Path $OutLog) { Start-Process 'notepad.exe' -ArgumentList $OutLog }
  else { [System.Windows.Forms.MessageBox]::Show('日志文件还没有生成', '农场桥接') }
})
$menu.MenuItems.Add($mLog) | Out-Null

$menu.MenuItems.Add((New-Object System.Windows.Forms.MenuItem -ArgumentList '-')) | Out-Null

$mAuto = New-Object System.Windows.Forms.MenuItem -ArgumentList ('开机自动启动（' + $(if (Get-AutoStart) { '已开启' } else { '未开启' }) + '）')
$mAuto.Checked = Get-AutoStart
$mAuto.Add_Click({
  $mAuto.Checked = -not $mAuto.Checked
  Set-AutoStart $mAuto.Checked
  $mAuto.Text = '开机自动启动（' + $(if ($mAuto.Checked) { '已开启' } else { '未开启' }) + '）'
  $notify.ShowBalloonTip(3000, '农场桥接',
    ('开机自动启动已' + ($(if ($mAuto.Checked) { '开启' } else { '关闭' }))),
    [System.Windows.Forms.ToolTipIcon]::Info)
})
$menu.MenuItems.Add($mAuto) | Out-Null

$mPause = New-Object System.Windows.Forms.MenuItem -ArgumentList '暂停服务'
$mPause.Add_Click({
  if ($script:Paused) {
    # 恢复：从暂停状态重新启动桥接
    $script:Paused = $false
    $mPause.Text = '暂停服务'
    $script:LastTryStart = Get-Date '2000-01-01'   # 解除 30s 冷却，立即拉起
    [void](Start-Bridge)
    Start-Sleep -Seconds 5
    Update-Status
  } else {
    # 暂停：停掉桥接，并阻止自动拉起
    $script:Paused = $true
    $mPause.Text = '恢复服务'
    Stop-Bridge
    Start-Sleep -Milliseconds 400
    Update-Status
  }
})
$menu.MenuItems.Add($mPause) | Out-Null

$mRestart = New-Object System.Windows.Forms.MenuItem -ArgumentList '重启服务'
$mRestart.Add_Click({
  $script:Paused = $false
  $mPause.Text = '暂停服务'
  $notify.Text = '农场桥接 重启中…'
  Stop-Bridge
  Start-Sleep -Seconds 2
  $script:LastTryStart = Get-Date '2000-01-01'   # 解除 30s 冷却，立即拉起
  [void](Start-Bridge)
  Start-Sleep -Seconds 6
  Update-Status
})
$menu.MenuItems.Add($mRestart) | Out-Null

$menu.MenuItems.Add((New-Object System.Windows.Forms.MenuItem -ArgumentList '-')) | Out-Null

$mExit = New-Object System.Windows.Forms.MenuItem -ArgumentList '退出'
$mExit.Add_Click({
  $notify.Visible = $false
  Stop-Bridge
  $timer.Stop()
  [System.Windows.Forms.Application]::Exit()
})
$menu.MenuItems.Add($mExit) | Out-Null

$notify.ContextMenu = $menu

# 双击图标 = 打开状态页
$notify.Add_DoubleClick({ Start-Process $StateURL })

# ---------- 状态刷新 ----------
function New-DotIcon([string]$hex) {
  # 画一个纯色圆点当托盘图标：颜色 = 状态，一眼能看出来
  $bmp = New-Object System.Drawing.Bitmap 32, 32
  $g   = [System.Drawing.Graphics]::FromImage($bmp)
  $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
  $g.Clear([System.Drawing.Color]::Transparent)
  $col = [System.Drawing.ColorTranslator]::FromHtml($hex)
  $b   = New-Object System.Drawing.SolidBrush $col
  $g.FillEllipse($b, 3, 3, 26, 26)
  $p   = New-Object System.Drawing.Pen ([System.Drawing.Color]::White), 2
  $g.DrawEllipse($p, 3, 3, 26, 26)
  $g.Dispose()
  $script:IconBmp = $bmp          # 必须留住引用，否则图标会变黑块
  return [System.Drawing.Icon]::FromHandle($bmp.GetHicon())
}

$script:LastLevel = ''           # 上次的状态分级，用来判断"变化了才提醒"

function Update-Status {
  $level  = 'down'               # down=桥接没起 / nobadge=工牌离线 / ok=正常
  $detail = '桥接未运行'
  $icon   = '#9E9E9E'            # 灰
  $head   = '桥接未运行'

  if (Test-BridgeAlive) {
    $detail = '桥接运行中'
    $icon   = '#FF9800'
    $level  = 'nobadge'
    $head   = '桥接运行中 · 工牌未连接'
    try {
      $r = Invoke-RestMethod -Uri $StateURL -TimeoutSec 3
      $badgeOn = $false
      if ($r.badge) { $badgeOn = [bool]$r.badge.online }
      if ($badgeOn) {
        $level = 'ok'
        $icon  = '#4CAF50'
        $viaTxt = if ($r.badge.via -eq 'usb') { 'USB' } else { '无线' }
        $head = "正常运行 · 工牌${viaTxt}在线"
        if ($r.badge.ip) { $head += " ($($r.badge.ip))" }
        if ($r.summary) {
          $s2 = $r.summary
          $detail = "打印中 $($s2.run) · 空闲 $($s2.idle) · 队列 $($s2.queue) · 共 $($s2.total) 台"
        }
      }
      elseif ($r.summary) {
        $s2 = $r.summary
        $detail = "打印中 $($s2.run) · 空闲 $($s2.idle) · 共 $($s2.total) 台"
      }
    } catch { }
  } elseif ($script:Paused) {
    $level  = 'paused'
    $icon   = '#607D8B'   # 蓝灰 = 已暂停
    $head   = '已暂停 · 右键「恢复服务」'
    $detail = '桥接已被手动暂停'
  } else {
    [void](Start-Bridge)
  }

  # 菜单第一行 + 悬停提示
  $mHead.Text = $head
  $mInfo.Text = "  $detail"
  if ($script:Paused) { $mPause.Text = '恢复服务' } else { $mPause.Text = '暂停服务' }
  $notify.Text = "$head`n$detail"
  try {
    $old = $notify.Icon
    $notify.Icon = New-DotIcon $icon
    if ($old) { $old.Dispose() }          # 不释放会攒几千个图标对象
  } catch { }

  # 状态变了才提醒一次（平时安静，不弹窗打扰）
  if ($level -ne $script:LastLevel) {
    if ($script:LastLevel -ne '') {
      $msg = switch ($level) {
        'ok'      { '桥接和工牌都在线了' }
        'nobadge' { '工牌没连上：检查工牌 WiFi 和是否开机' }
        'down'    { '桥接没在运行，正在重试拉起' }
        'paused'  { '桥接已暂停（右键「恢复服务」重新开始）' }
      }
      $notify.ShowBalloonTip(4000, '农场桥接', $msg, [System.Windows.Forms.ToolTipIcon]::Info)
    }
    $script:LastLevel = $level
    Write-TLog "状态变化 -> $level / $head"
  }
}

$timer = New-Object System.Windows.Forms.Timer
$timer.Interval = 10000
$timer.Add_Tick({ Update-Status })
$timer.Start()

# 首次启动
[void](Start-Bridge)
Start-Sleep -Seconds 8
Update-Status

[System.Windows.Forms.Application]::Run()
