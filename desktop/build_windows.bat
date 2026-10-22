@echo off
rem ============================================================
rem  MoonCheck Windows 单文件构建
rem  需要 conda 环境 mooncheck-desktop（含 pyinstaller）：
rem    conda create -n mooncheck-desktop python=3.12 -y
rem    conda run -n mooncheck-desktop python -m pip install -r requirements-build.txt
rem ============================================================
setlocal
cd /d "%~dp0"

where conda >nul 2>nul
if errorlevel 1 (
  echo [FAIL] 未找到 conda。请先安装 Miniconda 或手动激活环境后运行：
  echo        python -m PyInstaller --clean --noconfirm mooncheck.spec
  exit /b 1
)

conda run -n mooncheck-desktop --no-capture-output python -m PyInstaller --clean --noconfirm --distpath dist\windows mooncheck.spec
if errorlevel 1 exit /b 1

copy /Y config.example.json dist\windows\config.example.json >nul
echo.
echo [OK] 产物：dist\windows\MoonCheck.exe（双击=图形界面；命令行参数=CLI）
echo      首次运行会在 exe 同目录生成 config.json
