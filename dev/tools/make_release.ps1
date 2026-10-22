#requires -Version 5.1
# 本文件含中文：请保存为「UTF-8 with BOM」，或者用同目录的 make_release.bat 启动
# （bat 会把本文件复制成带 BOM 的临时副本再执行，避免 Windows PowerShell 5.1 按 GBK 解码）
<#
.SYNOPSIS
  make_release.ps1 —— 发行包打包 + 上传（**不编译**）

  双击同目录的 make_release.bat 即可运行。它只做四件事：
    1. 从 firmware/include/app_version.h 自动读取版本号（FW_VERSION）
    2. 校验 firmware/.pio/build/<env>/ 里已有的 build 产物（缺了就报错，不会替你编译）
    3. 用 esptool merge_bin 生成两个型号的出厂合并镜像 + 两个四文件全量 zip + sha256.txt + notes.md；
   若项目根下有 desktop/，同时附带桌面版 MoonCheck 可执行文件（Windows exe + Linux 二进制）；
   Release 正文写到 notes-<版本>.md，不覆盖仓库自带的 notes.md 发行史
    4. 用 gh 自动创建/更新 GitHub Release 并上传全部资产（没有 gh 时打印手动上传指引）

  可选参数（双击一般不需要）：
    -Version 0.4.0      覆盖自动读到的版本号
    -NoUpload           只打包，不上传
    -Prerelease         Release 标记为预发布
    -AllowStale         允许 build 产物比源码旧（默认拒绝，防止发出错版本的包）
    -Repo owner/name    指定 gh 目标仓库（默认用脚本所在 git 仓库）
    -Only s3            只打包一个型号（s3 / wroom32，按 Key 子串匹配）
#>
[CmdletBinding()]
param(
    [string]$Version = '',
    [switch]$NoUpload,
    [switch]$Prerelease,
    [switch]$AllowStale,
    [switch]$SkipDesktop,
    [string]$Repo = '',
    [string]$Only = ''
)

$ErrorActionPreference = 'Stop'

function Fail([string]$msg) {
    Write-Host ''
    Write-Host '[失败] ' -ForegroundColor Red -NoNewline
    Write-Host $msg
    exit 1
}
function Step([string]$msg) {
    Write-Host ''
    Write-Host ('--- ' + $msg) -ForegroundColor Cyan
}
function Info([string]$msg) { Write-Host ('      ' + $msg) }
function Warn([string]$msg) { Write-Host ('[警告] ' + $msg) -ForegroundColor Yellow }
function FileSize([string]$path) {
    if (Test-Path -LiteralPath $path) { (Get-Item -LiteralPath $path).Length } else { 0 }
}
function Human([int64]$bytes) {
    '{0:N0} B' -f $bytes
}

Step '第 0 步：定位项目与工具'

# .bat 会把脚本复制成带 BOM 的临时副本再运行，此时用环境变量定位真正的项目根
$Base = $null
if ($env:WANAN_RELEASE_ROOT) {
    $item = Get-Item -LiteralPath $env:WANAN_RELEASE_ROOT -ErrorAction SilentlyContinue
    if ($item) { $Base = $item.FullName }
}
if (-not $Base) { $Base = Split-Path -Parent $PSScriptRoot }
# 脚本可能位于 <项目>/tools/（主工程）或 <项目>/dev/tools/（公开副本）；
# 找不到 firmware 目录就向上逐级查找（最多 3 级）。
$tries = 0
while ($tries -lt 3 -and -not (Test-Path -LiteralPath (Join-Path $Base 'firmware'))) {
    $up = Split-Path -Parent $Base
    if (-not $up -or $up -eq $Base) { break }
    $Base = $up
    $tries += 1
}
if (-not (Test-Path -LiteralPath (Join-Path $Base 'firmware'))) {
    Fail "在 $Base 没找到 firmware 目录。这个脚本必须放在 <项目>/tools/ 或 <项目>/dev/tools/ 下运行。"
}
Set-Location -LiteralPath $Base
Info "项目根目录：$Base"

# ---- build 产物目录（mooncheck 这类公开副本里没有 .pio，自动回退到主工程） ----
$ownBuild = Join-Path $Base 'firmware\.pio\build'
$BuildRoot = $null
$parentDir = Split-Path -Parent $Base
foreach ($cand in @($ownBuild,
                    (Join-Path $parentDir 'firmware\.pio\build'),
                    (Join-Path $parentDir 'mooncheck\firmware\.pio\build'),
                    (Join-Path $parentDir 'MoonCheck\firmware\.pio\build'))) {
    if (Test-Path -LiteralPath $cand) { $BuildRoot = $cand; break }
}
if (-not $BuildRoot) {
    Fail ("找不到 build 产物目录（期望 firmware\.pio\build）。`n      本脚本不编译，请先在 VSCode/PlatformIO 里 Build 两个环境：`n        pio run -d firmware -e esp32-s3-devkitc-1`n        pio run -d firmware -e esp32-wroom-32-COM6`n      检查过的位置：$ownBuild 与 " + (Join-Path $parentDir 'firmware\.pio\build'))
}
Info ("build 产物目录：$BuildRoot")
if ($BuildRoot -ne $ownBuild) {
    Warn ("本副本内没有 .pio 构建目录，已改用主工程的：$BuildRoot")
}
# build 归属于哪个工程：<build 目录>\..\.pio\.. = <项目>\firmware
$ProjFw = Split-Path -Parent (Split-Path -Parent $BuildRoot)
Info ("源码目录：$ProjFw")

# ---- 版本号（自动获取） ----
$verFile = Join-Path $ProjFw 'include\app_version.h'
if (-not (Test-Path -LiteralPath $verFile)) { Fail ("缺少版本头文件：$verFile") }
$ver = $Version
if (-not $ver) {
    # 逐行匹配，不用多行正则：先找含 FW_VERSION 的行，再取该行第一个引号串
    foreach ($ln in (Get-Content -LiteralPath $verFile)) {
        if (($ln -match 'FW_VERSION') -and ($ln -match '"([^"]+)"')) {
            $ver = $Matches[1]
            break
        }
    }
}
if (-not $ver) {
    $preview = ((Get-Content -LiteralPath $verFile | Select-Object -First 20) -join ' | ')
    if ($preview.Length -gt 200) { $preview = ($preview -replace '^(.{0,200}).*$', '$1') + '…' }
    Fail ('app_version.h 里没找到 FW_VERSION 的定义（期望形如 #define FW_VERSION "0.3.1"）：' + $verFile + '；文件前 20 行：' + $preview)
}
if ($ver -notmatch '^[0-9]+\.[0-9]+[0-9A-Za-z.\-]*$') {
    Fail "版本号格式不合理：'$ver'（应形如 0.3.1 或 1.0.0-rc1）"
}
$tag = 'v' + $ver
Info "固件版本：$ver   （tag: $tag，来源 app_version.h）"

# ---- 桌面单文件版（可选资产：项目根下有 desktop/ 才打包） ----
$Desk = Join-Path $Base 'desktop'
$dver = ''
$deskWin = $null
$deskLinux = $null
if ($SkipDesktop) {
    Info '已按 -SkipDesktop 跳过桌面版可执行文件'
    $Desk = $null
} elseif (-not (Test-Path -LiteralPath (Join-Path $Desk 'mooncheck.spec'))) {
    Warn "在 $Base 下没有 desktop/（主工程没有桌面版），本次只发固件产物"
    $Desk = $null
} else {
    foreach ($ln in (Get-Content -LiteralPath (Join-Path $Desk 'src\mooncheck_app\__init__.py'))) {
        if ($ln -match "__version__\s*=\s*'([^']+)'") { $dver = $Matches[1]; break }
    }
    if (-not $dver) { Fail '读不到 desktop\src\mooncheck_app\__init__.py 里的 __version__' }
    $deskWin = Join-Path $Desk 'dist\windows\MoonCheck.exe'
    $deskLinux = Join-Path $Desk 'dist\linux\MoonCheck'
    Info ("桌面版：v$dver（将随本次 Release 附带 Windows/Linux 两个可执行文件）")
}
$relTitle = if ($Desk) { "MoonCheck $ver（免编译刷写 + 桌面版）" } else { "MoonCheck $ver（免编译刷写版）" }

# ---- esptool / python / boot_app0 ----
$pioHome = Join-Path $env:USERPROFILE '.platformio'
$esptoolPy = Join-Path $pioHome 'packages\tool-esptoolpy\esptool.py'
$bootApp0 = Join-Path $pioHome 'packages\framework-arduinoespressif32\tools\partitions\boot_app0.bin'

$py = $null
foreach ($cand in @((Join-Path $pioHome 'penv\Scripts\python.exe'), (Join-Path $pioHome 'penv\bin\python'))) {
    if (Test-Path -LiteralPath $cand) { $py = $cand; break }
}
if (-not $py) {
    $g = Get-Command python -ErrorAction SilentlyContinue
    if ($g) { $py = $g.Source }
}
if (-not $py) { Fail '找不到 python。请先安装 PlatformIO Core（它自带 penv）。' }

if (Test-Path -LiteralPath $esptoolPy) {
    $esptBase = @($esptoolPy)
} else {
    & $py -m esptool version *> $null
    if ($LASTEXITCODE -ne 0) {
        Fail "既没有 $esptoolPy，python 也导入不了 esptool。请先跑一次 pio run（会自动装 tool-esptoolpy）。"
    }
    $esptBase = @('-m', 'esptool')
}
if (-not (Test-Path -LiteralPath $bootApp0)) {
    Fail "缺少 boot_app0.bin：$bootApp0`n      （它来自 arduinoespressif32 框架包；没装框架说明从没编译过）"
}
Info "python  ：$py"
Info "esptool ：$($esptBase -join ' ')"
Info "boot_app0：$bootApp0"

# ---- 源码新鲜度（防止把旧 build 当新版发出去） ----
$srcNewest = $null
$src = Get-ChildItem -Path $ProjFw -Recurse -File -Include *.cpp, *.h, *.ini -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -notmatch '\\\.pio\\' } |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ($src) { $srcNewest = $src }
if ($srcNewest) {
    Info ("最新源码：{0}  {1}" -f $srcNewest.Name, $srcNewest.LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss'))
}

# ---- 两个目标 ----
$Targets = @(
    @{
        Key       = 's3-n16r8'
        Name      = 'ESP32-S3 N16R8（16 MB Flash）'
        Chip      = 'esp32s3'
        Freq      = '80m'
        FSize     = '16MB'
        BootOff   = '0x0'
        Envs      = @('esp32-s3-devkitc-1')
        Prefix    = 'esp32-s3-'
        BoardMark = 'ESP32-S3'
    },
    @{
        Key       = 'wroom32-4mb'
        Name      = '经典 ESP32 WROOM-32（4 MB Flash）'
        Chip      = 'esp32'
        Freq      = '40m'
        FSize     = '4MB'
        BootOff   = '0x1000'
        Envs      = @('esp32-wroom-32-COM6', 'esp32-wroom-32-COM7', 'esp32-wroom-32')
        Prefix    = 'esp32-wroom-32'
        BoardMark = 'ESP32'
    }
)

Step '第 1 步：检查 build 产物（本脚本不编译）'

function Resolve-EnvDir($t) {
    foreach ($e in $t.Envs) {
        $d = Join-Path $BuildRoot $e
        if (Test-Path -LiteralPath (Join-Path $d 'firmware.bin')) { return $d }
    }
    if (Test-Path -LiteralPath $BuildRoot) {
        $hit = Get-ChildItem -LiteralPath $BuildRoot -Directory -ErrorAction SilentlyContinue |
            Where-Object { ($_.Name -like ($t.Prefix + '*')) -and (Test-Path -LiteralPath (Join-Path $_.FullName 'firmware.bin')) } |
            Sort-Object LastWriteTime -Descending | Select-Object -First 1
        if ($hit) { return $hit.FullName }
    }
    return $null
}

$plan = @()
$skipped = @()
foreach ($t in $Targets) {
    if ($Only -and ($t.Key -notlike ('*' + $Only + '*'))) {
        Info ("按 -Only {0} 跳过：{1}" -f $Only, $t.Name)
        $skipped += $t.Name
        continue
    }
    $envDir = Resolve-EnvDir $t
    if (-not $envDir) {
        $avail = ''
        $root = $BuildRoot
        if (Test-Path -LiteralPath $root) {
            $avail = (Get-ChildItem -LiteralPath $root -Directory | ForEach-Object { $_.Name }) -join ', '
        }
        Fail ("{0} 没有可用的 build 产物。`n      期望目录：{1}`n      现有环境：{2}`n      请先编译：pio run -d firmware -e {3}" -f `
            $t.Name, $BuildRoot, $avail, $t.Envs[0])
    }
    $envName = Split-Path -Leaf $envDir
    $missing = @()
    foreach ($f in @('bootloader.bin', 'partitions.bin', 'firmware.bin')) {
        if (-not (Test-Path -LiteralPath (Join-Path $envDir $f))) { $missing += $f }
    }
    if ($missing) {
        Fail ("{0}（环境 {1}）缺少产物：{2}`n      请重新编译：pio run -d firmware -e {1}" -f $t.Name, $envName, ($missing -join ', '))
    }
    $fw = Join-Path $envDir 'firmware.bin'
    $fwItem = Get-Item -LiteralPath $fw
    Info ("{0}  [env {1}]" -f $t.Name, $envName)
    Info ("      bootloader {0} | partitions {1} | firmware {2}  (built {3})" -f `
        (Human (FileSize (Join-Path $envDir 'bootloader.bin'))),
        (Human (FileSize (Join-Path $envDir 'partitions.bin'))),
        (Human (FileSize $fw)),
        $fwItem.LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss'))

    # 产物比源码旧 => 版本号/改动可能没进镜像
    $stale = [bool]($srcNewest -and ($srcNewest.LastWriteTime -gt $fwItem.LastWriteTime))
    $staleMsg = ''
    if ($stale) {
        $staleMsg = ("build 产物比源码旧（{0} 在 {1} 之后被改过）" -f $srcNewest.Name, $fwItem.LastWriteTime.ToString('MM-dd HH:mm'))
    }
    # 版本字符串应在镜像里（main.cpp 开机打印 firmware <ver>）；在二进制里搜，
    # 搜索本身出错就当「无法判定」，绝不让它单独决定要不要中止发行
    $verState = 'unknown'
    try {
        if (Select-String -LiteralPath $fw -Pattern ('firmware ' + $ver) -SimpleMatch -Encoding ascii -Quiet) {
            $verState = 'found'
        } elseif (Select-String -LiteralPath $fw -Pattern $ver -SimpleMatch -Encoding ascii -Quiet) {
            $verState = 'found'
        } else {
            $verState = 'missing'
        }
    } catch { $verState = 'unknown' }

    if ($stale -and ($verState -eq 'missing')) {
        # 既过期又在镜像里找不到新版本号：发出去大概率货不对板
        if ($AllowStale) { Warn ("$staleMsg，且镜像里搜不到版本号 $ver —— 已用 -AllowStale 强制继续") }
        else { Fail ("$staleMsg，而且镜像里搜不到版本号 $ver。`n      请先重新 Build：pio run -d firmware -e $envName`n      确认无误要硬发，再加 -AllowStale") }
    }
    elseif ($stale) { Warn ("$staleMsg；镜像里能找到版本号 $ver（或无法判定），若那笔改动需要生效请重新 Build") }
    elseif ($verState -eq 'missing') { Warn "在 firmware.bin 里没搜到版本字符串 $ver（可能被优化掉了，继续但请自查）" }

    $plan += @{ T = $t; EnvDir = $envDir; EnvName = $envName; Fw = $fw }
}
if ($plan.Count -eq 0) {
    Fail ("-Only '{0}' 没匹配到任何型号（可选 s3 / wroom32），没有要打包的东西" -f $Only)
}

Step "第 2 步：准备 release\ 目录（增量打包：保留历史版本产物）"

$Rel = Join-Path $Base 'release'
if (-not (Test-Path -LiteralPath $Rel)) {
    New-Item -ItemType Directory -Path $Rel | Out-Null
}
# release/ 里有已入库（git 追踪）的历史发行产物；这里只清理**本次版本**的文件和临时解包目录，
# 绝不整体删除，避免打包一次就把上一版从工作区里抹掉。
foreach ($f in @(Get-ChildItem -LiteralPath $Rel -File -ErrorAction SilentlyContinue)) {
    if (($f.Name -match ('^wanan-' + [regex]::Escape($ver) + '-')) -or
        ($dver -and ($f.Name -match ('^MoonCheck-' + [regex]::Escape($dver) + '-'))) -or
        ($f.Name -eq 'sha256.txt') -or ($f.Name -eq ('notes-' + $ver + '.md'))) {
        Remove-Item -LiteralPath $f.FullName -Force
    }
}
foreach ($d in @(Get-ChildItem -LiteralPath $Rel -Directory -ErrorAction SilentlyContinue)) {
    if ($d.Name -match '-full$') { Remove-Item -LiteralPath $d.FullName -Recurse -Force }
}
Info "输出目录：$Rel（历史版本保留，只重建 $ver 的产物）"

Step '第 3 步：生成发行资产（合并镜像 + 全量 zip + 校验和）'

$assets = @()
$rows = @()
foreach ($p in $plan) {
    $t = $p.T
    $merged = Join-Path $Rel ('wanan-' + $ver + '-' + $t.Key + '-merged.bin')
    $zip = Join-Path $Rel ('wanan-' + $ver + '-' + $t.Key + '-full.zip')

    $bl = Join-Path $p.EnvDir 'bootloader.bin'
    $pt = Join-Path $p.EnvDir 'partitions.bin'

    Info ("{0} -> merge_bin ({1}, dio/{2}/{3})" -f $t.Name, $t.Chip, $t.Freq, $t.FSize)
    $mergeArgs = @('--chip', $t.Chip, 'merge_bin', '-o', $merged,
        '-fm', 'dio', '-ff', $t.Freq, '-fs', $t.FSize, '-t', '0x0',
        $t.BootOff, $bl, '0x8000', $pt, '0xe000', $bootApp0, '0x10000', $p.Fw)
    & $py @esptBase @mergeArgs
    if ($LASTEXITCODE -ne 0) { Fail "merge_bin 失败（退出码 $LASTEXITCODE）：$($t.Name)" }
    if ((FileSize $merged) -le 0) { Fail "合并镜像为空：$merged" }
    Info ("      merged = {0}" -f (Human (FileSize $merged)))

    $stage = Join-Path $Rel ($t.Key + '-full')
    New-Item -ItemType Directory -Path $stage -Force | Out-Null
    Copy-Item -LiteralPath $bl -Destination (Join-Path $stage 'bootloader.bin')
    Copy-Item -LiteralPath $pt -Destination (Join-Path $stage 'partitions.bin')
    Copy-Item -LiteralPath $bootApp0 -Destination (Join-Path $stage 'boot_app0.bin')
    Copy-Item -LiteralPath $p.Fw -Destination (Join-Path $stage 'firmware.bin')

    $flashTxt = @(
        ('wanan-{0}-{1} 全量包 —— 烧录地址表（{2}）' -f $ver, $t.Key, $t.Name),
        '',
        '用 https://espressif.github.io/esptool-js/ 时，按下面每一行「添加」一条记录，然后 Program。',
        ('刷之前先确认芯片型号是 {0}，别用另一个型号的包。' -f $t.BoardMark),
        '',
        '地址        文件',
        ('{0,-11} bootloader.bin' -f $t.BootOff),
        '0x8000      partitions.bin',
        '0xe000      boot_app0.bin',
        '0x10000     firmware.bin',
        '',
        '只升级固件（保留 WiFi 账号、坐标、日程等配置）时：',
        '  只填一行 —— 地址 0x10000，文件 firmware.bin。其余地址一律别动。',
        '刷了 -merged.bin（0x0）等于整片重置，蓝牙里配过的东西会全部丢失，需要重新配置。',
        '',
        'esptool-js 里 Flash Mode/Frequency/Size 选 keep/keep/detect。',
        '如果停在 "Hard resetting via RTS pin..."：点 Disconnect 或关掉网页，再按 RST / 拔插 USB。',
        '刷写本身已经成功了（前面有 Hash of data verified），那是网页工具的收尾问题。'
    ) -join "`r`n"
    $flashTxt | Set-Content -LiteralPath (Join-Path $stage 'FLASH.txt') -Encoding utf8

    Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -Force
    if (-not (Test-Path -LiteralPath $zip)) { Fail "zip 生成失败：$zip" }
    Info ("      full.zip = {0}" -f (Human (FileSize $zip)))

    $assets += $merged
    $assets += $zip
    $rows += ('| wanan-{0}-{1}-merged.bin | {2} | {3} | 整片出厂镜像，刷在 `0x0` |' -f `
        $ver, $t.Key, (Human (FileSize $merged)), $t.Name)
    $rows += ('| wanan-{0}-{1}-full.zip | {2} | {3} | 四文件+FLASH.txt，升级只刷 `0x10000` |' -f `
        $ver, $t.Key, (Human (FileSize $zip)), $t.Name)
}

# ---- 桌面版可执行文件：检查新鲜度后拷进 release\ ----
if ($Desk) {
    foreach ($b in @($deskWin, $deskLinux)) {
        if (-not (Test-Path -LiteralPath $b)) {
            Fail ("缺少桌面版可执行文件：$b`n      请先构建：`n        Windows：cd desktop && conda run -n mooncheck-desktop python -m PyInstaller --clean --noconfirm --distpath dist\windows mooncheck.spec`n        Linux：在 WSL 里 cd desktop && bash build_linux.sh`n      确认不要桌面版，则加 -SkipDesktop")
        }
    }
    $deskSrcNewest = Get-ChildItem -Path (Join-Path $Desk 'src') -Recurse -File -Include *.py -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    $specFile = Join-Path $Desk 'mooncheck.spec'
    if ($deskSrcNewest -and (Test-Path -LiteralPath $specFile) -and
        ((Get-Item -LiteralPath $specFile).LastWriteTime -gt $deskSrcNewest.LastWriteTime)) {
        $deskSrcNewest = Get-Item -LiteralPath $specFile
    }
    if ($deskSrcNewest) {
        foreach ($b in @($deskWin, $deskLinux)) {
            if ($deskSrcNewest.LastWriteTime -gt (Get-Item -LiteralPath $b).LastWriteTime) {
                if ($AllowStale) { Warn ("桌面源码（$($deskSrcNewest.Name)）比可执行文件 $b 新 —— 已用 -AllowStale 强制继续") }
                else { Fail ("桌面源码 $($deskSrcNewest.Name)（改动于 $($deskSrcNewest.LastWriteTime)）比可执行文件 $b 新。`n      发出去的就是旧桌面版。请先重建两个可执行文件（命令见上），确认要硬发再加 -AllowStale") }
            }
        }
    }
    $dw = Join-Path $Rel ('MoonCheck-' + $dver + '-windows-x64.exe')
    $dl = Join-Path $Rel ('MoonCheck-' + $dver + '-linux-x86_64')
    Copy-Item -LiteralPath $deskWin -Destination $dw -Force
    Copy-Item -LiteralPath $deskLinux -Destination $dl -Force
    $assets += $dw
    $assets += $dl
    $rows += ('| MoonCheck-{0}-windows-x64.exe | {1} | Windows 10/11 x64 | 双击 = 图形界面；命令行子命令 = CLI |' -f `
        $dver, (Human (FileSize $dw)))
    $rows += ('| MoonCheck-{0}-linux-x86_64 | {1} | x86-64 Linux | 下载后 `chmod +x` 再运行 |' -f `
        $dver, (Human (FileSize $dl)))
    Info ("桌面版已入包：{0} / {1}" -f (Split-Path -Leaf $dw), (Split-Path -Leaf $dl))
}

Step '第 4 步：SHA256 与 Release 正文'

$hashLines = Get-ChildItem -LiteralPath $Rel -File |
    Where-Object {
        (($_.Extension -in @('.bin', '.zip')) -and ($_.Name -match ('^wanan-' + [regex]::Escape($ver) + '-'))) -or
        ($dver -and ($_.Name -match ('^MoonCheck-' + [regex]::Escape($dver) + '-')))
    } |
    Sort-Object Name |
    ForEach-Object {
        $h = (Get-FileHash -Algorithm SHA256 -LiteralPath $_.FullName).Hash.ToLower()
        '{0}  {1}' -f $h, $_.Name
    }
$shaFile = Join-Path $Rel 'sha256.txt'
$hashLines | Set-Content -LiteralPath $shaFile -Encoding ascii
Info ("写入 {0}（{1} 条）" -f $shaFile, $hashLines.Count)
foreach ($line in $hashLines) { Info ('      ' + $line) }

$assetTable = ($rows -join "`r`n")
$hashBlock = ($hashLines -join "`r`n")
if ($skipped.Count -gt 0) {
    $includeNames = (($plan | ForEach-Object { $_.T.Name }) -join '、')
    $skipNames = ($skipped -join '、')
    $chipLine = ('**注意：本次只发布了 ' + $includeNames + '，跳过了 ' + $skipNames + '，不要下错型号。**')
} else {
    $chipLine = '**先看芯片丝印再下载，两个包不能互换：**'
}
$chipLines = @()
foreach ($p in $plan) {
    if ($p.T.Key -eq 's3-n16r8') {
        $chipLines += ('- 芯片丝印含 **ESP32-S3**（16 MB Flash）-> `wanan-' + $ver + '-s3-n16r8-merged.bin`')
    } else {
        $chipLines += ('- 芯片丝印是 **ESP32**（WROOM-32 / 4 MB Flash）-> `wanan-' + $ver + '-wroom32-4mb-merged.bin`')
    }
}
$chipBlock = ($chipLines -join "`r`n")
$deskNotesBlock = ''
if ($Desk) {
    $deskNotesBlock = @(
        '### 桌面版（不需要开发板，电脑上直接跑）',
        '',
        ('- `MoonCheck-{0}-windows-x64.exe` —— Windows 10/11 64-bit：双击 = 图形界面；带命令行参数 = CLI。首次运行在 exe 同目录生成 `config.json`' -f $dver),
        ('- `MoonCheck-{0}-linux-x86_64` —— x86-64 Linux：下载后先 `chmod +x MoonCheck-{0}-linux-x86_64`，用法与 Windows 版一致' -f $dver),
        '- 多账号、打卡时间窗、每人独立推送与固件同语义；使用说明见仓库 `desktop/README.md`',
        ''
    ) -join "`r`n"
}
$deskBuildLine = ''
if ($Desk) { $deskBuildLine = ('- 桌面版：v{0}（PyInstaller 单文件，Windows/Linux 各自平台构建后随包发布）' -f $dver) }
$notesLines = @(
    '## 免编译刷写（不用装任何工具）'
    ''
    $chipLine
    ''
    $chipBlock
    ''
    '### 刷入步骤'
    ''
    '1. 用 **Chrome / Edge**（必须支持 Web Serial）打开 <https://espressif.github.io/esptool-js/>'
    '2. Connect 选中串口，Baudrate 先用 `921600`（卡住或报错就退回 `115200`）'
    '3. Flash Address 填 `0x0`，文件选上面那个 `-merged.bin`'
    '4. Program，等到日志出现 `Hash of data verified` 就已经写好了'
    '5. **如果停在** `Hard resetting via RTS pin...`：点 **Disconnect**（或直接关掉这个网页），再按板上 RST / 拔插 USB。'
    '   这是网页工具的收尾问题，不是刷写失败；USB 桥占着串口的 RTS 会把 EN 一直拉低，此时按 RST 是没反应的。'
    '6. 第一次开机是**未配置**状态：合并包会擦掉 NVS。蓝牙会广播 `WANAN-XXXX`（XXXX = 蓝牙 MAC 后两字节），LED 慢闪。'
    '   用手机 nRF Connect 或本仓库 `tools/ble_console.py` 连上，按 docs/quickstart.md 配齐（刷写细节见 docs/flashing.md）。'
    ''
    '### 本版本资产'
    ''
    '| 文件 | 大小 | 适用 | 用途 |'
    '| --- | --- | --- | --- |'
    $assetTable
    ''
    '### 只想升级固件？'
    ''
    '下载 `-full.zip`，**只**把里面的 `firmware.bin` 填到地址 `0x10000` 刷入 —— WiFi、账号、坐标、日程都会保留。'
    '刷 `0x0` 的合并包等于整片重置，配置会全部丢失。'
    ''
    $deskNotesBlock
    '### SHA256'
    ''
    '```'
    $hashBlock
    '```'
    ''
    '### 本次构建信息'
    ''
    ('- 固件版本：`' + $ver + '`（取自 `firmware/include/app_version.h` 的 `FW_VERSION`，由打包脚本自动读取）')
    '- 打包方式：`make_release.ps1`（主工程 `tools/`，公开副本 `dev/tools/`；**只消费已有 build 产物，不重新编译**）'
    $deskBuildLine
    '- 已知工具链：platform espressif32@7.1.3 / PlatformIO Core 6.2.0 / esptool 4.11.0 / NimBLE-Arduino 2.5.1 / ArduinoJson 7.4.3'
    '- 详细图文说明：docs/flashing.md（网页刷写）与 docs/quickstart.md（蓝牙配置）'
    ''
    '> 打卡时间窗 21:00-23:30，登录只在窗口内发生；配置完成后 `auto on` 才会每天自动执行。'
)
$notes = ($notesLines -join "`r`n")
$notesFile = Join-Path $Rel ('notes-' + $ver + '.md')
$notes | Set-Content -LiteralPath $notesFile -Encoding utf8
Info "Release 正文：$notesFile（仓库自带的 notes.md 发行史不受影响）"

Step '第 5 步：上传 GitHub Release'

$uploadFiles = $assets + @($shaFile)

if ($NoUpload) {
    Info '已按 -NoUpload 跳过上传。手动上传：'
    Info ('  gh release create ' + $tag + ' --title "' + $relTitle + '" --notes-file ' + $notesFile)
    Info ('  cd 到仓库目录后逐个把这些文件拖进 Release 页面：')
    foreach ($f in $uploadFiles) { Info ('    ' + $f) }
    Write-Host ''
    Write-Host '打包完成（未上传）。' -ForegroundColor Green
    exit 0
}

# 选仓库目录：优先 mooncheck（公开仓库副本），其次脚本所在仓库
$repoDir = $null
foreach ($cand in @((Join-Path $Base 'mooncheck'), $Base)) {
    if (Test-Path -LiteralPath (Join-Path $cand '.git')) { $repoDir = $cand; break }
}
if (-not $repoDir) {
    Warn '没找到 .git 目录，无法自动上传。请先在要发布的目录里 git init / 关联远程仓库，再重跑。'
    Warn ('候选位置：{0}、{1}' -f (Join-Path $Base 'mooncheck'), $Base)
    Write-Host ''
    Write-Host '打包完成（未上传，因为还没有 git 仓库）。' -ForegroundColor Green
    exit 0
}

$ghCmd = Get-Command gh -ErrorAction SilentlyContinue
if (-not $ghCmd) {
    Warn ('没装 GitHub CLI（gh），不能自动上传。仓库目录：' + $repoDir)
    Warn ('手动方式 A：安装 gh（winget install GitHub.cli --accept-source-agreements --accept-package-agreements）后重跑本脚本，即可自动上传。')
    Warn ('手动方式 B：打开仓库的 Releases -> Draft a new release -> tag 输入 ' + $tag + ' -> 正文粘贴 ' + $notesFile + ' 的内容 -> 拖入下面这些文件 -> Publish release。')
    foreach ($f in $uploadFiles) { Info ('    ' + $f) }
    exit 0
}

& gh auth status *> $null
if ($LASTEXITCODE -ne 0) {
    Fail 'gh 未登录。先在一个终端里执行 gh auth login（需要 repo 作用域），再重跑本脚本。'
}

# 本机可能残留坏代理（例如 HTTPS_PROXY 指向没在跑的 127.0.0.1:7890），上传时临时清掉
$savedProxy = @{}
foreach ($name in @('HTTP_PROXY', 'HTTPS_PROXY', 'ALL_PROXY', 'NO_PROXY', 'http_proxy', 'https_proxy', 'all_proxy', 'no_proxy')) {
    $item = Get-Item -LiteralPath ("env:" + $name) -ErrorAction SilentlyContinue
    if ($item) { $savedProxy[$name] = $item.Value; Remove-Item -LiteralPath ("env:" + $name) -ErrorAction SilentlyContinue }
}

Push-Location -LiteralPath $repoDir
try {
    # gh/git 把探测结果和进度写在 stderr 上（比如查一个还不存在的 Release）；
    # Stop 模式下 PowerShell 5.1 会把这些 stderr 行升级成 NativeCommandError 终止脚本。
    # 上传段整体改用 Continue，成败一律看 $LASTEXITCODE。
    $ErrorActionPreference = 'Continue'
    Info ("仓库目录：$repoDir")

    $exists = $false
    & gh release view $tag *> $null
    if ($LASTEXITCODE -eq 0) { $exists = $true }

    if ($exists) {
        Info ("Release $tag 已存在 -> 覆盖上传资产")
        & gh release upload $tag --clobber @uploadFiles
        if ($LASTEXITCODE -ne 0) { Fail "gh release upload 失败（退出码 $LASTEXITCODE）" }
    } else {
        # 本地有同名 tag 就先推，避免 gh 把 Release 建到别的提交上
        $localTag = $false
        & git rev-parse -q --verify ("refs/tags/" + $tag) *> $null
        if ($LASTEXITCODE -eq 0) { $localTag = $true }
        if (-not $localTag) {
            & git tag $tag
            if ($LASTEXITCODE -ne 0) { Warn ("git tag $tag 没建成（仓库可能还没有任何提交），gh 会自己建 tag") }
        }
        if ($localTag) {
            & git push origin $tag *> $null
            if ($LASTEXITCODE -ne 0) { Warn "git push origin $tag 失败（离线或无权限）；gh 会自行创建远端 tag" }
        }
        $createArgs = @('release', 'create', $tag, '--title', $relTitle, '--notes-file', $notesFile)
        if ($Prerelease) { $createArgs += '--prerelease' }
        if ($Repo) { $createArgs = @('-R', $Repo) + $createArgs }
        Info ("创建 Release $tag 并上传 {0} 个资产" -f $uploadFiles.Count)
        & gh @createArgs @uploadFiles
        if ($LASTEXITCODE -ne 0) { Fail "gh release create 失败（退出码 $LASTEXITCODE）" }
    }

    $url = ''
    $viewArgs = @('release', 'view', $tag, '--json', 'url')
    if ($Repo) { $viewArgs = @('-R', $Repo) + $viewArgs }
    $json = (& gh @viewArgs | Out-String)
    if ($json -match 'https?://[^"\s]+') { $url = $Matches[0] }
    Write-Host ''
    Write-Host '上传完成。' -ForegroundColor Green
    if ($url) { Write-Host ('  ' + $url) -ForegroundColor Green }
    foreach ($f in $uploadFiles) { Info ('  已上传 ' + (Split-Path -Leaf $f)) }
} finally {
    $ErrorActionPreference = 'Stop'
    Pop-Location
    foreach ($name in $savedProxy.Keys) { Set-Item -LiteralPath ("env:" + $name) -Value $savedProxy[$name] }
}
