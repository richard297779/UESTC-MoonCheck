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
    [switch]$NoUpload,
    [switch]$Upload,
    [switch]$Prerelease,
    [switch]$SkipDesktop,
    [switch]$SkipAndroid,
    [string]$AndroidApk = '',
    [ValidatePattern('^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$')][string]$Repo = 'richard297779/UESTC-MoonCheck',
    [ValidateSet('', 's3', 'wroom32')][string]$Only = ''
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
    $targets = @(
        @{ Key='s3-n16r8'; Env='esp32-s3-devkitc-1'; Chip='esp32s3'; Boot='0x0'; Freq='80m'; Size='16MB' },
        @{ Key='wroom32-4mb'; Env='esp32-wroom-32'; Chip='esp32'; Boot='0x1000'; Freq='40m'; Size='4MB' }
    )
    if ($Only) { $targets = @($targets | Where-Object { $_.Key.StartsWith($Only) }) }
    $pio = Join-Path $env:USERPROFILE '.platformio'
    $python = Join-Path $pio 'penv/Scripts/python.exe'
    $esptool = Join-Path $pio 'packages/tool-esptoolpy/esptool.py'
    $bootApp = Join-Path $pio 'packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin'
    foreach ($required in @($python, $esptool, $bootApp)) { if (-not (Test-Path -LiteralPath $required)) { throw "缺少 PlatformIO 工具：$required" } }
    $fwSources = @(Get-ChildItem -LiteralPath 'firmware/src','firmware/include','firmware/partitions' -Recurse -File)
    $fwSources += @(Get-Item -LiteralPath 'firmware/platformio.ini','VERSION')
    foreach ($target in $targets) {
        $build = Join-Path $Base ('firmware/.pio/build/' + $target.Env)
        $app = Join-Path $build 'firmware.bin'
        Assert-Fresh $app $fwSources
        $binaryText = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($app))
        if (-not $binaryText.Contains($ver)) { throw "固件内找不到版本 $ver：$app" }
        foreach ($file in @('bootloader.bin','partitions.bin')) { if (-not (Test-Path -LiteralPath (Join-Path $build $file))) { throw "缺少 $file：$build" } }
        $merged = Join-Path $Rel ("wanan-$ver-" + $target.Key + '-merged.bin')
        $merge = @($esptool,'--chip',$target.Chip,'merge_bin','-o',$merged,'-fm','dio','-ff',$target.Freq,'-fs',$target.Size,'-t','0x0',
            $target.Boot,(Join-Path $build 'bootloader.bin'),'0x8000',(Join-Path $build 'partitions.bin'),'0xe000',$bootApp,'0x10000',$app)
        Native $python $merge | ForEach-Object { Write-Host $_ }
        # Unique stage per invocation; historical release files are never deleted.
        $stage = Join-Path $Rel ('.stage/' + $target.Key + '-' + [Guid]::NewGuid().ToString('N'))
        New-Item -ItemType Directory -Force -Path $stage | Out-Null
        foreach ($file in @('bootloader.bin','partitions.bin','firmware.bin')) { Copy-Item -LiteralPath (Join-Path $build $file) -Destination (Join-Path $stage $file) }
        Copy-Item -LiteralPath $bootApp -Destination (Join-Path $stage 'boot_app0.bin')
        @(
            "MoonCheck $ver / $($target.Key)",
            "$($target.Boot) bootloader.bin",'0x8000 partitions.bin','0xe000 boot_app0.bin','0x10000 firmware.bin','',
            '升级且分区表不变：只刷 firmware.bin 到 0x10000，保留配置。',
            '首次安装：对应 merged.bin 刷到 0x0，会清空配置。',
            '网页工具：https://espressif.github.io/esptool-js/；两个板型的产物不能混用。'
        ) | Set-Content -LiteralPath (Join-Path $stage 'FLASH.txt') -Encoding UTF8
        $zip = Join-Path $Rel ("wanan-$ver-" + $target.Key + '-full.zip')
        Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -Force
        $assets += @($merged, $zip)
        $rows += "| $([IO.Path]::GetFileName($merged)) | $($target.Key) 出厂镜像；0x0，会清配置 |"
        $rows += "| $([IO.Path]::GetFileName($zip)) | $($target.Key) 四文件包；升级只刷 firmware.bin 到 0x10000 |"
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
    Write-Host '桌面版 1.0.0 未支持微信票据，不随本次发行重新打包。'
    $guide = Join-Path $Rel 'PACKAGE.md'
    @(
        "# MoonCheck $ver 发行包",'',
        'Android：安装 android-debug.apk，允许该来源安装，然后打开应用连接自己的 WANAN 设备。',
        '这是 Debug 签名测试版，未做真实手机 BLE 联调；不包含独立手机打卡引擎。',
        '固件：确认板型；首次 merged.bin 刷 0x0 会清配置；同分区升级用 full.zip 的 firmware.bin 刷 0x10000。',
        '微信授权由每位账号本人准备；先配置、体检与取票测试，再主动开启自动任务。',
        '仅在本人受控的近距离环境配置；固件当前未提供 BLE 配对加密或应用鉴权。',
        '同一 Debug 签名可覆盖升级；签名不同需卸载旧测试版。凭据不保存在手机，设备配置保留。',
        '桌面旧版本尚不支持微信票据，本发行包不包含桌面程序。',
        ('完整指南：https://github.com/' + $Repo + '/blob/' + $tag + '/docs/quickstart.md'),
        '核验：manifest 中列出内部各文件的 SHA-256，GitHub 单独资产 sha256.txt 还包含整包 ZIP 的摘要。'
    ) | Set-Content -LiteralPath $guide -Encoding UTF8
    $assets += $guide
    $manifest = [ordered]@{
        version=$ver; repository=$Repo; sourceCommit=$commit; sourceDirty=$sourceDirty; firmwareProtocol=1; maxCommandBytes=4095;
        android=$androidMetadata; boards=@($targets | ForEach-Object { $_.Key });
        validation='Automated build and offline tests; no physical BLE or live-account validation';
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
        "# MoonCheck $ver：Android 控制端与固件兼容更新",'',
        '本版新增原生 Android BLE 控制端：按键操作、配置向导、多人槽位、通知管理、诊断与可离线搜索的帮助。',
        '固件修复长命令被 511 字节队列截断的问题，hello 声明 maxCommandBytes=4095。',
        'PC 控制台修复中文跨分片与长命令写入，并扩大敏感字段隐藏范围。',
        'Android 修复配置保存后的查询超时处理，避免结果未确认时继续发送指令。',
        '', '## 下载选择', '', '| 文件 | 用途 |','| --- | --- |', ($rows -join "`n"),
        "| MoonCheck-$ver-release.zip | 整套资产：两板型固件、APK、使用说明与清单 |",'',
        '首次刷固件：对应 merged.bin 刷 0x0，会清空已有配置。同分区升级：full.zip 内 firmware.bin 只刷 0x10000。',
        'APK 为 Debug 签名测试版，支持 Android 8+；不会替代 ESP32，手机不需要常驻。',
        '尚未完成真实手机/开发板联调、正式签名及安装升级矩阵，不应理解为完整生产验收。',
        '当前 BLE 配置链路未配置配对/加密/设备鉴权，请在本人受控的近距离环境使用。',
        '桌面版尚未实现微信票据，本版不重新发行桌面二进制。', '',
        '## 构建与溯源', "- 源码提交：$commit", "- 固件与 APK：$ver；BLE 协议 1",
        '- SHA-256：见 sha256.txt；ZIP 内清单见 release-manifest.json。',
        '- 验证结果：见 docs/project-health.md；应用说明见 android/README.md。',
        '- 网页刷写：https://espressif.github.io/esptool-js/',
        "- 使用指南：https://github.com/$Repo/blob/$tag/docs/quickstart.md"
    )
    $notes | Set-Content -LiteralPath $notesFile -Encoding UTF8
    Write-Host "已生成 $Rel；共 $($assets.Count + 1) 个上传资产。"
    if ($Upload) {
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
        $ghArgs = @('release','create',$tag,'-R',$Repo,'--verify-tag','--target',$commit,'--draft','--title',"MoonCheck $ver（固件 + Android 测试版）",'--notes-file',$notesFile)
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
        Native $gh @('release','edit',$tag,'-R',$Repo,'--draft=false') | ForEach-Object { Write-Host $_ }
        Native $gh @('release','view',$tag,'-R',$Repo,'--json','url') | ForEach-Object { Write-Host $_ }
    } else { Write-Host '未上传。提交并推送后，用相同参数加 -Upload 显式创建发行。' }
} catch {
    Write-Error $_ -ErrorAction Continue
    exit 1
} finally {
    $env:JAVA_HOME = $previousJava
    Set-Location -LiteralPath $originalLocation
}
