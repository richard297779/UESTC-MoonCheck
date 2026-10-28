param([switch]$SkipTests)
$ErrorActionPreference = 'Stop'
$saved = @{}
foreach ($name in @('PYTHONPATH','PYINSTALLER_CONFIG_DIR','TEMP','TMP')) { $saved[$name] = [Environment]::GetEnvironmentVariable($name,'Process') }
Push-Location $PSScriptRoot
try {
    $dependencies = Join-Path $PSScriptRoot '.build-tools/python-deps'
    if (Test-Path -LiteralPath $dependencies) { $env:PYTHONPATH = $dependencies }
    $env:PYINSTALLER_CONFIG_DIR = Join-Path $PSScriptRoot 'build/pyinstaller-cache'
    $buildTemp = Join-Path $PSScriptRoot 'build/tmp'
    New-Item -ItemType Directory -Force -Path $buildTemp | Out-Null
    $env:TEMP = $buildTemp; $env:TMP = $buildTemp
    if (-not $SkipTests) {
        python -m unittest discover -s tests -v
        if ($LASTEXITCODE -ne 0) { throw '桌面离线测试失败' }
    }
    python -m PyInstaller --clean --noconfirm --distpath dist/windows mooncheck.spec
    if ($LASTEXITCODE -ne 0) { throw '桌面程序构建失败' }
    python package_local.py
    if ($LASTEXITCODE -ne 0) { throw '冻结程序冒烟或打包校验失败' }
    Write-Host '产物：desktop/dist/windows/MoonCheck.exe'
} finally {
    foreach ($name in $saved.Keys) { [Environment]::SetEnvironmentVariable($name,$saved[$name],'Process') }
    Pop-Location
}
