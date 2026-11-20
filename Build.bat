@echo off
setlocal EnableExtensions DisableDelayedExpansion

rem Run without arguments to keep the console open after a double-click.
set "build_pause="
if "%~1"=="" set "build_pause=1"

pushd "%~dp0"
if errorlevel 1 (
    echo ERROR: Could not open the OpenFrame source directory.
    if defined build_pause pause
    exit /b 1
)

"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\build.ps1" -Package %*
set "build_result=%ERRORLEVEL%"
popd

if "%build_result%"=="0" (
    echo OpenFrame build completed successfully.
) else (
    echo ERROR: OpenFrame build failed. Exit code: %build_result%
)
if defined build_pause pause
exit /b %build_result%
