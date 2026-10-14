<#
.SYNOPSIS
  清理 Windows 缓存的 BLE 设备（GATT）记录。

.DESCRIPTION
  症状：bleak / nRF Connect 报
        "Could not get GATT services: Unreachable" 或服务发现超时。
  原因：Windows 会为每个连接过的 BLE 设备缓存"设备节点 + GATT 记录"。
        固件改动广播布局后，旧缓存可能失效，导致连上却读不到服务表。
  常规解法是「设置 → 蓝牙和其他设备 → 删除设备」，但**未配对或信号弱时设置里看不到该设备**，
  此时用本脚本按设备名或 MAC 直接删除设备节点（等价于清缓存）。

.PARAMETER Mac
  设备 MAC，例如 50:78:7D:15:98:9D（可省略，默认按名称匹配）。

.PARAMETER Name
  设备名关键字，默认 WANAN。

.PARAMETER Remove
  真正执行删除（需要**管理员** PowerShell）。不加此开关时只查看。

.EXAMPLE
  # 1) 只查看（普通权限即可）
  powershell -ExecutionPolicy Bypass -File tools\clear_ble_cache.ps1

  # 2) 删除（管理员 PowerShell）
  powershell -ExecutionPolicy Bypass -File tools\clear_ble_cache.ps1 -Mac 50:78:7D:15:98:9D -Remove
#>
param(
  [string]$Mac = '',
  [string]$Name = 'WANAN',
  [switch]$Remove
)

function Show-Items([string]$title, $items) {
  Write-Host "`n=== $title ===" -ForegroundColor Cyan
  if (-not $items) { Write-Host '（无）'; return }
  $items | Select-Object Status, Class, FriendlyName, InstanceId |
    Format-Table -AutoSize | Out-String -Width 200 | Write-Host
}

$macHex = ($Mac -replace '[:-]', '').ToUpper()
$all = Get-PnpDevice -ErrorAction SilentlyContinue
$byName = @($all | Where-Object { $_.FriendlyName -like "*$Name*" })
$byMac = @()
if ($macHex) { $byMac = @($all | Where-Object { $_.InstanceId -like "*$macHex*" }) }
$bleNodes = @($all | Where-Object { $_.InstanceId -like 'BTHLE\*' })

Show-Items "名称含 '$Name' 的设备" $byName
if ($macHex) { Show-Items "InstanceId 含 '$macHex' 的设备" $byMac }
Show-Items '系统缓存的 BLE 设备节点（BTHLE\*）' $bleNodes

$targets = @($byName + $byMac | Sort-Object InstanceId -Unique)

if (-not $Remove) {
  Write-Host "`n以上仅为查看。要删除，请用【管理员 PowerShell】重新运行并加 -Remove" -ForegroundColor Yellow
  Write-Host '  例：powershell -ExecutionPolicy Bypass -File tools\clear_ble_cache.ps1 -Remove' -ForegroundColor Yellow
  exit 0
}

if (-not $targets) { Write-Host "`n没有可删除的目标。" -ForegroundColor Yellow; exit 0 }

foreach ($d in $targets) {
  Write-Host "`n删除 $($d.FriendlyName) [$($d.InstanceId)] ..." -ForegroundColor Green
  $out = & pnputil.exe /remove-device "$($d.InstanceId)" 2>&1
  if ($LASTEXITCODE -ne 0) {
    Write-Host '  pnputil 失败（多半是权限不足），改用 Remove-PnpDevice ...' -ForegroundColor DarkYellow
    Remove-PnpDevice -InstanceId $d.InstanceId -Confirm:$false -ErrorAction SilentlyContinue
  } else {
    $out | Write-Host
  }
}

Write-Host "`n重启蓝牙服务，清空运行时状态 ..." -ForegroundColor Green
Restart-Service bthserv -Force -ErrorAction SilentlyContinue
Write-Host '完成。请重新运行： python tools\ble_console.py --timeout 60' -ForegroundColor Green
