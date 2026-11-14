@echo off
rem Build the x64-only installer from the current output\ staging.
rem Prereq: build64.bat has been run  (output\ contains the new binaries + data).
setlocal
cd /d "%~dp0"
if not exist env.bat copy env.bat.template env.bat >nul
if exist env.bat call env.bat
if not defined WEASEL_VERSION set WEASEL_VERSION=0.17.4
if not defined WEASEL_BUILD set WEASEL_BUILD=0
if not defined PRODUCT_VERSION set PRODUCT_VERSION=%WEASEL_VERSION%.%WEASEL_BUILD%
set "MAKENSIS=C:\Users\huangqi\source\repos\Qeynos\nsis\NSIS\makensis.exe"
if not exist "%MAKENSIS%" set "MAKENSIS=C:\Program Files (x86)\NSIS\Bin\makensis.exe"
if not exist "%MAKENSIS%" echo [FAILED] makensis.exe not found & exit /b 1
if not exist "output\install-x64.nsi" echo [FAILED] output\install-x64.nsi not found & exit /b 1
pushd output
"%MAKENSIS%" /V2 /DWEASEL_VERSION=%WEASEL_VERSION% /DWEASEL_BUILD=%WEASEL_BUILD% /DPRODUCT_VERSION=%PRODUCT_VERSION% install-x64.nsi
set RC=%ERRORLEVEL%
popd
if not "%RC%"=="0" echo [FAILED] installer build failed & exit /b 1
echo.
echo [OK] output\archives\weasel-%PRODUCT_VERSION%-installer.exe
dir /b "output\archives\weasel-%PRODUCT_VERSION%-installer.exe"
exit /b 0
