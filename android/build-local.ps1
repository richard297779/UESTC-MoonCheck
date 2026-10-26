param([switch]$SkipTests)
$ErrorActionPreference = 'Stop'
$savedEnvironment = @{}
foreach ($variable in @('JAVA_HOME', 'GRADLE_USER_HOME', 'TEMP', 'TMP', 'GRADLE_OPTS')) {
    $savedEnvironment[$variable] = [Environment]::GetEnvironmentVariable($variable, 'Process')
}
Push-Location $PSScriptRoot
try {
    if (-not $env:JAVA_HOME) {
        $localJdk = Get-ChildItem -LiteralPath '.build-tools/jdk' -Directory -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($localJdk) { $env:JAVA_HOME = $localJdk.FullName }
    }
    if (-not $env:JAVA_HOME -or -not (Test-Path -LiteralPath "$env:JAVA_HOME/bin/javac.exe")) {
        throw '请安装 JDK 17，并将 JAVA_HOME 指向 JDK 根目录。也可使用 Android Studio 的兼容 JDK。'
    }
    $env:GRADLE_USER_HOME = Join-Path $PSScriptRoot '.gradle-home'
    $buildTemp = Join-Path $PSScriptRoot 'build/local-temp'
    New-Item -ItemType Directory -Force $buildTemp | Out-Null
    $env:TEMP = $buildTemp
    $env:TMP = $buildTemp
    $env:GRADLE_OPTS = "-Djava.io.tmpdir=$buildTemp"
    $buildTasks = if ($SkipTests) { @('assembleDebug') } else { @('testDebugUnitTest', 'lintDebug', 'assembleDebug', 'assembleDebugAndroidTest') }
    $arguments = @('--no-daemon', "-Dorg.gradle.jvmargs=-Xmx3g -Dfile.encoding=UTF-8 -Djava.io.tmpdir=$buildTemp", '-Pkotlin.compiler.execution.strategy=in-process') + $buildTasks
    $gradleExecutable = if (Test-Path -LiteralPath '.build-tools/gradle-8.11.1/bin/gradle.bat') {
        './.build-tools/gradle-8.11.1/bin/gradle.bat'
    } else { './gradlew.bat' }
    & $gradleExecutable @arguments
    if ($LASTEXITCODE -ne 0) { throw "构建失败，退出码 $LASTEXITCODE" }
    Write-Host 'APK: app/build/outputs/apk/debug/app-debug.apk'
} finally {
    foreach ($variable in $savedEnvironment.Keys) {
        [Environment]::SetEnvironmentVariable($variable, $savedEnvironment[$variable], 'Process')
    }
    Pop-Location
}
