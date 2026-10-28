#requires -Version 5.1
<#
.SYNOPSIS
打包已验证的固件与 Android Debug APK。默认只打包，上传须显式指定 -Upload。
.EXAMPLE
./dev/tools/make_release.ps1 -NoUpload
./dev/tools/make_release.ps1 -Upload -Prerelease -Repo owner/repository
#>
[CmdletBinding()]
param(
    [string]$Version = '',
    [string]$ValidationNote = '',
    [switch]$NoUpload,
    [switch]$Upload,
    [switch]$Prerelease,
    [switch]$SkipDesktop,
    [switch]$SkipAndroid,
    [string]$AndroidApk = '',
    [ValidatePattern('^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$')][string]$Repo = 'richard297779/UESTC-MoonCheck',
    [string]$Only = '',
    [switch]$IncludeExperimental
)
$ErrorActionPreference = 'Stop'
function Native([string]$File, [string[]]$Arguments) {
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $result = & $File @Arguments 2>&1
    $code = $LASTEXITCODE
    $ErrorActionPreference = $previous
    if ($code -ne 0) { throw "$File failed ($code): $($result -join [Environment]::NewLine)" }
    return ($result | ForEach-Object { $_.ToString() })
}
function Assert-Fresh([string]$Artifact, [System.IO.FileInfo[]]$Sources) {
    if (-not (Test-Path -LiteralPath $Artifact)) { throw "缺少产物：$Artifact；请先构建。" }
    if ((Get-Item -LiteralPath $Artifact).Length -eq 0) { throw "空产物：$Artifact" }
    $newer = @($Sources | Where-Object { $_.LastWriteTimeUtc -gt (Get-Item -LiteralPath $Artifact).LastWriteTimeUtc })
    if ($newer.Count -gt 0) { throw "产物比源码旧：$Artifact；请重建（例如 $($newer[0].Name) 已变化）。" }
}
function Digest([string]$Path) { return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }
$originalLocation = Get-Location
$previousJava = $env:JAVA_HOME
try {
    $Base = if ($env:WANAN_RELEASE_ROOT) { $env:WANAN_RELEASE_ROOT } else { $PSScriptRoot }
    for ($i = 0; $i -lt 4 -and -not (Test-Path -LiteralPath (Join-Path $Base 'VERSION')); $i++) { $Base = Split-Path -Parent $Base }
    $Base = (Resolve-Path -LiteralPath $Base).Path
    Set-Location -LiteralPath $Base
    $ver = (Get-Content -LiteralPath 'VERSION' -Raw).Trim()
    if ($ver -notmatch '^\d+\.\d+\.\d+$') { throw 'VERSION 必须为 X.Y.Z。' }
    if ($Version -and $Version -ne $ver) { throw '-Version 与 VERSION 不同，拒绝给旧产物换版本标签。' }
    $fwHeader = Get-Content -LiteralPath 'firmware/include/app_version.h' -Raw
    if ($fwHeader -notmatch ('#define FW_VERSION "' + [regex]::Escape($ver) + '"')) { throw '固件 FW_VERSION 与 VERSION 不一致。' }
    $commit = (Native 'git' @('rev-parse', 'HEAD') | Select-Object -Last 1).Trim()
    $sourceDirty = [bool](Native 'git' @('status', '--porcelain'))
    if ($Upload -and $NoUpload) { throw '-Upload 与 -NoUpload 不能同时使用。' }
    if ($Upload -and $sourceDirty) { throw '上传前必须提交工作区改动，确保标签、清单和源码一致。' }
    $tag = "v$ver"
    $Rel = Join-Path $Base "release/$tag"
    New-Item -ItemType Directory -Force -Path $Rel | Out-Null
    $assets = @()
    $rows = @()
    $selection = @('tools/board_targets.py', '--validate-builds')
    if ($Only) { $selection += @('--only', $Only) }
    if ($IncludeExperimental) { $selection += '--include-experimental' }
    $catalog = (Native 'python' $selection | Out-String | ConvertFrom-Json)
    $targets = @($catalog | ForEach-Object {
        @{ Key=$_.id; Env=$_.env; Chip=$_.chip; Boot=$_.bootOffset; Freq=$_.flashFreq; Size=$_.flashSize; Profile=$_ }
    })
    $pio = Join-Path $env:USERPROFILE '.platformio'
    $python = Join-Path $pio 'penv/Scripts/python.exe'
    $esptool = Join-Path $pio 'packages/tool-esptoolpy/esptool.py'
    $bootApp = Join-Path $pio 'packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin'
    foreach ($required in @($python, $esptool, $bootApp)) { if (-not (Test-Path -LiteralPath $required)) { throw "缺少 PlatformIO 工具：$required" } }
    $fwSources = @(Get-ChildItem -LiteralPath 'firmware/src','firmware/include','firmware/partitions','firmware/scripts' -Recurse -File)
    $fwSources += @(Get-Item -LiteralPath 'firmware/platformio.ini','firmware/board-targets.json','VERSION')
    foreach ($target in $targets) {
        $build = Join-Path $Base ('firmware/.pio/build/' + $target.Env)
        $app = Join-Path $build 'firmware.bin'
        Assert-Fresh $app $fwSources
        $binaryText = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($app))
        if (-not $binaryText.Contains($ver)) { throw "固件内找不到版本 $ver：$app" }
        foreach ($file in @('bootloader.bin','partitions.bin')) { if (-not (Test-Path -LiteralPath (Join-Path $build $file))) { throw "缺少 $file：$build" } }
        $buildMetadata = Get-Content -LiteralPath (Join-Path $build 'board-build.json') -Raw | ConvertFrom-Json
        $profile = $target.Profile
        $merged = Join-Path $Rel ("wanan-$ver-" + $target.Key + '-merged.bin')
        $merge = @($esptool,'--chip',$target.Chip,'merge_bin','-o',$merged,'-fm',$profile.flashMode,'-ff',$target.Freq,'-fs',$target.Size,'-t','0x0',
            $target.Boot,(Join-Path $build 'bootloader.bin'),$profile.partitionOffset,(Join-Path $build 'partitions.bin'),$profile.bootAppOffset,$bootApp,$profile.appOffset,$app)
        Native $python $merge | ForEach-Object { Write-Host $_ }
        # Unique stage per invocation; historical release files are never deleted.
        $stage = Join-Path $Base ('release/.staging/' + $tag + '/' + $target.Key + '-' + [Guid]::NewGuid().ToString('N'))
        New-Item -ItemType Directory -Force -Path $stage | Out-Null
        foreach ($file in @('bootloader.bin','partitions.bin','firmware.bin')) { Copy-Item -LiteralPath (Join-Path $build $file) -Destination (Join-Path $stage $file) }
        Copy-Item -LiteralPath $bootApp -Destination (Join-Path $stage 'boot_app0.bin')
        @(
            "MoonCheck $ver / $($target.Key)",
            "$($target.Boot) bootloader.bin","$($profile.partitionOffset) partitions.bin","$($profile.bootAppOffset) boot_app0.bin","$($profile.appOffset) firmware.bin",'',
            "硬件验证：$($profile.physicalValidation)",
            '新增板型未经过实际验证；欢迎向开发者提供板卡标记、内存规格、版本与脱敏测试结果。','',
            '升级且分区表不变：只刷 firmware.bin 到 0x10000，保留配置。',
            '首次安装：对应 merged.bin 刷到 0x0，会清空配置。',
            '网页工具：https://espressif.github.io/esptool-js/；不同板型的产物不能混用。'
        ) | Set-Content -LiteralPath (Join-Path $stage 'FLASH.txt') -Encoding UTF8
        $zip = Join-Path $Rel ("wanan-$ver-" + $target.Key + '-full.zip')
        Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -Force
        $assets += @($merged, $zip)
        $validationBadge = if ($profile.status -eq 'experimental') { '；实验构建，未经实际验证' } else { '' }
        $rows += "| $([IO.Path]::GetFileName($merged)) | $($target.Key) 出厂镜像；0x0，会清配置$validationBadge |"
        $rows += "| $([IO.Path]::GetFileName($zip)) | $($target.Key) 四文件包；升级只刷 firmware.bin 到 0x10000$validationBadge |"
    }
    $androidMetadata = $null
    if (-not $SkipAndroid) {
        if (-not $AndroidApk) { $AndroidApk = Join-Path $Base 'android/app/build/outputs/apk/debug/app-debug.apk' }
        $androidSources = @(Get-ChildItem -LiteralPath 'android/app/src/main','android/gradle' -Recurse -File)
        $androidSources += @(Get-Item -LiteralPath 'android/app/build.gradle.kts','android/build.gradle.kts','android/settings.gradle.kts','android/gradle.properties','VERSION')
        Assert-Fresh $AndroidApk $androidSources
        $sdk = $env:ANDROID_HOME
        if (-not $sdk) {
            $local = Join-Path $Base 'android/local.properties'
            if (Test-Path -LiteralPath $local) {
                $sdkLine = Get-Content -LiteralPath $local | Where-Object { $_ -match '^sdk.dir=' } | Select-Object -First 1
                if ($sdkLine) { $sdk = ($sdkLine -replace '^sdk.dir=','').Replace('\\','\').Replace('\:',':') }
            }
        }
        $sdkTools = Join-Path $sdk 'build-tools/35.0.0'
        if (-not $env:JAVA_HOME) {
            $jdkFolder = Join-Path $Base 'android/.build-tools/jdk'
            if (Test-Path -LiteralPath $jdkFolder) { $env:JAVA_HOME = (Get-ChildItem -LiteralPath $jdkFolder -Directory | Select-Object -First 1).FullName }
        }
        $badging = (Native (Join-Path $sdkTools 'aapt.exe') @('dump','badging',$AndroidApk)) -join "`n"
        if ($badging -notmatch ("versionName='" + [regex]::Escape($ver) + "'")) { throw 'APK 内部版本号与 VERSION 不一致。' }
        if ($badging -notmatch "package: name='io.mooncheck.app'") { throw '不是 MoonCheck APK。' }
        if ($badging -notmatch "versionCode='(\d+)'") { throw 'APK 缺少 versionCode。' }
        $versionCode = [int]$Matches[1]
        Native (Join-Path $sdkTools 'apksigner.bat') @('verify','--verbose',$AndroidApk) | ForEach-Object { Write-Host $_ }
        $apk = Join-Path $Rel "MoonCheck-$ver-android-debug.apk"
        Copy-Item -LiteralPath $AndroidApk -Destination $apk -Force
        $assets += $apk
        $rows += "| $([IO.Path]::GetFileName($apk)) | Android 8+ BLE 控制端；Debug 签名测试版 |"
        $androidMetadata = @{ version=$ver; versionCode=$versionCode; package='io.mooncheck.app'; buildType='debug'; minSdk=26; targetSdk=35 }
    }
    Write-Host '桌面 1.1.0 独立构建和验收，不随本次固件发行重新打包。'
    $guide = Join-Path $Rel 'PACKAGE.md'
    @(
        "# MoonCheck $ver 发行包",'',
        'Android：安装 android-debug.apk，允许该来源安装，然后打开应用连接自己的 WANAN 设备。',
        '这是 Debug 签名构建，不包含独立手机打卡引擎；联调验收状态见仓库 docs/project-health.md。',
        '固件：确认板型；首次 merged.bin 刷 0x0 会清配置；同分区升级用 full.zip 的 firmware.bin 刷 0x10000。',
        '微信授权由每位账号本人准备；先配置、体检与取票测试，再主动开启自动任务。',
        '维护者仅具备 S3-N16R8/WROOM-32 实测条件；其他四种新增目标未经实际验证。',
        ('联调记录：' + $(if ($ValidationNote) { $ValidationNote } else { '未提供本次确认；详见 docs/project-health.md，不能推断全部板型已验证。' })),
        ('欢迎验证者向开发者提交板卡规格、版本和脱敏结果：https://github.com/' + $Repo + '/issues'),
        ('所选板型：' + (($targets | ForEach-Object { $_.Key + ' [' + $_.Profile.physicalValidation + ']' }) -join '；')),
        '仅在本人受控的近距离环境配置；固件当前未提供 BLE 配对加密或应用鉴权。',
        '同一 Debug 签名可覆盖升级；签名不同需卸载旧测试版。凭据不保存在手机，设备配置保留。',
        '桌面 1.1.0 有独立微信票据实现，本固件发行包不包含桌面程序。',
        ('完整指南：https://github.com/' + $Repo + '/blob/' + $tag + '/docs/quickstart.md'),
        '核验：manifest 中列出内部各文件的 SHA-256，GitHub 单独资产 sha256.txt 还包含整包 ZIP 的摘要。'
    ) | Set-Content -LiteralPath $guide -Encoding UTF8
    $assets += $guide
    $manifest = [ordered]@{
        schemaVersion=2; toolchain=@{platform='espressif32 7.1.3';arduino='2.0.17';nimble='2.5.1';arduinoJson='7.4.3'}; version=$ver; repository=$Repo; sourceCommit=$commit; sourceDirty=$sourceDirty; firmwareProtocol=1; maxCommandBytes=4095;
        android=$androidMetadata; boards=@($targets | ForEach-Object { $_.Key });
        boardProfiles=@($targets | ForEach-Object {
            $p = $_.Profile
            $m = Get-Content -LiteralPath (Join-Path $Base ('firmware/.pio/build/' + $_.Env + '/board-build.json')) -Raw | ConvertFrom-Json
            @{ id=$p.id; env=$p.env; chip=$p.chip; flashSize=$p.flashSize; flashMode=$p.flashMode; flashFreq=$p.flashFreq; psram=$p.psram; status=$p.status;
               physicalValidation=$p.physicalValidation; partitionSha256=$m.partitionSha256; offsets=$m.offsets;
               partitions=(Get-Content -LiteralPath (Join-Path 'firmware' $p.partitionFile) -Raw) }
        });
        validation=$(if ($ValidationNote) { $ValidationNote } else { 'Automated checks; physical validation scope is documented in docs/project-health.md' });
        files=@($assets | ForEach-Object { [ordered]@{name=[IO.Path]::GetFileName($_);size=(Get-Item -LiteralPath $_).Length;sha256=(Digest $_)} })
    }
    $manifestFile = Join-Path $Rel 'release-manifest.json'
    $manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $manifestFile -Encoding UTF8
    $assets += $manifestFile
    $bundle = Join-Path $Rel "MoonCheck-$ver-release.zip"
    Compress-Archive -LiteralPath $assets -DestinationPath $bundle -Force
    $assets += $bundle
    $shaFile = Join-Path $Rel 'sha256.txt'
    $assets | Sort-Object | ForEach-Object { (Digest $_) + '  ' + [IO.Path]::GetFileName($_) } | Set-Content -LiteralPath $shaFile -Encoding ASCII
    $notesFile = Join-Path $Rel "notes-$ver.md"
    $notes = @(
        "# MoonCheck $ver：多板型与运行状态",'',
        '本版新增多板型构建、APK 运行状态（网络名、内存峰值、PSRAM）及脱敏开发者日志。',
        '修复状态 JSON 嵌套序列化覆盖前缀导致 APK 连接报回执不完整；已有用户更新固件即可，APK 无需重装。',
        '仅 S3-N16R8 与 WROOM-32 可由维护者实测；其余四种新增目标未经实机验证，欢迎提供脱敏反馈。',
        ('联调记录：' + $(if ($ValidationNote) { $ValidationNote } else { '见 docs/project-health.md；不推断逐板型兼容性。' })),
        '新增板型目录、单核适配、分区与芯片校验；继续保持协议 1 和 maxCommandBytes=4095。',
        '运行指标由设备分配器提供，开机后重新统计；日志仅当前连接保留，不持久化或导出。',
        '实验板型须显式选择；C6 与未明确硬件版本的 SuperMini 暂不提供产物。',
        '', '## 下载选择', '', '| 文件 | 用途 |','| --- | --- |', ($rows -join "`n"),
        "| MoonCheck-$ver-release.zip | 整套资产：所选板型固件、APK、使用说明与清单 |",'',
        '首次刷固件：对应 merged.bin 刷 0x0，会清空已有配置。同分区升级：full.zip 内 firmware.bin 只刷 0x10000。',
        'APK 为 Debug 签名测试版，支持 Android 8+；不会替代 ESP32，手机不需要常驻。',
        '功能联调验收状态见 docs/project-health.md；完整兼容矩阵和正式签名发布需独立核验。',
        '当前 BLE 配置链路未配置配对/加密/设备鉴权，请在本人受控的近距离环境使用。',
        '桌面 1.1.0 独立构建，本固件发行不重新打包桌面二进制。', '',
        '## 构建与溯源', "- 源码提交：$commit", "- 固件与 APK：$ver；BLE 协议 1",
        '- SHA-256：见 sha256.txt；ZIP 内清单见 release-manifest.json。',
        '- 验证结果：见 docs/project-health.md；应用说明见 android/README.md。',
        '- 网页刷写：https://espressif.github.io/esptool-js/',
        "- 使用指南：https://github.com/$Repo/blob/$tag/docs/quickstart.md"
    )
    $notes | Set-Content -LiteralPath $notesFile -Encoding UTF8
    Write-Host "已生成 $Rel；共 $($assets.Count + 1) 个上传资产。"
    if ($Upload) {
        Native 'python' @('tools/check_project.py','--release-dir',$Rel,'--require-clean') | ForEach-Object { Write-Host $_ }
        $gh = (Get-Command gh -ErrorAction Stop).Source
        $null = Native $gh @('api',"repos/$Repo")
        $remoteCommit = (Native $gh @('api',"repos/$Repo/commits/main",'--jq','.sha') | Select-Object -Last 1).Trim()
        if ($remoteCommit -ne $commit) { throw '先推送 main 到目标仓库；远端提交与当前 HEAD 不同。' }
        $tagRef = "refs/tags/$tag"
        if (-not (Native 'git' @('tag','--list',$tag))) { $null = Native 'git' @('tag','-a',$tag,'-m',"MoonCheck $ver",$commit) }
        $tagCommit = (Native 'git' @('rev-list','-n','1',$tag) | Select-Object -Last 1).Trim()
        if ($tagCommit -ne $commit) { throw '本地同名标签指向另一个提交，拒绝移动标签。' }
        # Verify origin belongs to the explicitly chosen repository before using its transport.
        $origin = (Native 'git' @('remote','get-url','origin') | Select-Object -Last 1).Trim()
        if ($origin -notmatch ('github\.com[:/]' + [regex]::Escape($Repo) + '(\.git)?$')) { throw 'origin 与 -Repo 不匹配。' }
        $null = Native 'git' @('push','origin',$tagRef)
        $ghArgs = @('release','create',$tag,'-R',$Repo,'--verify-tag','--target',$commit,'--draft','--title',"MoonCheck $ver（多板型固件 + Android 控制端）",'--notes-file',$notesFile)
        if ($Prerelease) { $ghArgs += '--prerelease' }
        $ghArgs += $assets + @($shaFile)
        Native $gh $ghArgs | ForEach-Object { Write-Host $_ }
        # Keep a draft if any asset is missing, truncated or uploaded with a different hash.
        $remote = (Native $gh @('release','view',$tag,'-R',$Repo,'--json','assets') | Out-String | ConvertFrom-Json).assets
        foreach ($asset in ($assets + @($shaFile))) {
            $name = [IO.Path]::GetFileName($asset)
            $match = @($remote | Where-Object { $_.name -eq $name })
            if ($match.Count -ne 1 -or $match[0].size -ne (Get-Item -LiteralPath $asset).Length -or $match[0].digest -ne ('sha256:' + (Digest $asset))) { throw "上传资产校验失败：$name；Release 保留为草稿。" }
        }
        $publishArgs = @('release','edit',$tag,'-R',$Repo,'--draft=false')
        if (-not $Prerelease) { $publishArgs += '--latest' }
        Native $gh $publishArgs | ForEach-Object { Write-Host $_ }
        Native $gh @('release','view',$tag,'-R',$Repo,'--json','url') | ForEach-Object { Write-Host $_ }
    } else { Write-Host '未上传。提交并推送后，用相同参数加 -Upload 显式创建发行。' }
} catch {
    Write-Error $_ -ErrorAction Continue
    exit 1
} finally {
    $env:JAVA_HOME = $previousJava
    Set-Location -LiteralPath $originalLocation
}
