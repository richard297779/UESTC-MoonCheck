@echo off
rem ==========================================================================
rem  make_release.bat -- double-click entry for make_release.ps1
rem
rem  Packages the ALREADY BUILT firmware artifacts (does NOT compile),
rem  auto-reads the version from firmware/include/app_version.h, then uploads
rem  a GitHub Release with gh.
rem
rem  Options pass straight through, e.g. from a terminal:
rem     make_release.bat -NoUpload
rem     make_release.bat -Only s3 -Prerelease
rem     make_release.bat -AllowStale -Version 0.4.0
rem
rem  Why copy through %TEMP% first: the .ps1 contains Chinese text. Windows
rem  PowerShell 5.1 only decodes a .ps1 as UTF-8 when the file starts with a
rem  BOM; without a BOM it falls back to the ANSI codepage, the Chinese turns
rem  into mojibake and string literals can even stop parsing. The copy re-reads
rem  the file explicitly as UTF-8 and writes it back with a BOM, so it works
rem  no matter how the source file was saved.
rem ==========================================================================
setlocal
set "SCRIPT_DIR=%~dp0"
for %%I in ("%SCRIPT_DIR%..") do set "WANAN_RELEASE_ROOT=%%~fI"
set "MR_SRC=%SCRIPT_DIR%make_release.ps1"
set "MR_RUN=%TEMP%\wanan_make_release.run.ps1"

cd /d "%WANAN_RELEASE_ROOT%"
echo ==========================================================================
echo  MoonCheck release packager (uses existing build output, does NOT build)
echo  project root : %WANAN_RELEASE_ROOT%
echo ==========================================================================

powershell -NoProfile -ExecutionPolicy Bypass -Command "if (-not (Test-Path -LiteralPath $env:MR_SRC)) { Write-Host ('[ERROR] make_release.ps1 not found next to this .bat: ' + $env:MR_SRC); exit 2 }; $c = Get-Content -Raw -Encoding UTF8 -LiteralPath $env:MR_SRC; Set-Content -LiteralPath $env:MR_RUN -Value $c -Encoding UTF8; & $env:MR_RUN %*; $rc = $LASTEXITCODE; Remove-Item -LiteralPath $env:MR_RUN -Force -ErrorAction SilentlyContinue; exit $rc"
set RC=%ERRORLEVEL%

echo.
if "%RC%"=="0" (
  echo [OK] finished successfully.
) else (
  echo [FAILED] exit code %RC%. Read the messages above - this tool never
  echo [FAILED] compiles: it only packages what is already in firmware\.pio\build.
)
echo.
pause
exit /b %RC%
