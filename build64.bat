@echo off
rem Build Win64 (x64) ONLY. Skips x86, librime, Boost building and the data steps.
rem Usage: build64.bat  [rebuild]
setlocal
cd /d "%~dp0"
if not exist env.bat copy env.bat.template env.bat >nul
if exist env.bat call env.bat
if not defined WEASEL_ROOT set WEASEL_ROOT=%CD%
if not defined VERSION_MAJOR set VERSION_MAJOR=0
if not defined VERSION_MINOR set VERSION_MINOR=17
if not defined VERSION_PATCH set VERSION_PATCH=4
if not defined WEASEL_VERSION set WEASEL_VERSION=%VERSION_MAJOR%.%VERSION_MINOR%.%VERSION_PATCH%
if not defined WEASEL_BUILD set WEASEL_BUILD=0
if not defined PRODUCT_VERSION set PRODUCT_VERSION=%WEASEL_VERSION%.%WEASEL_BUILD%
if not defined FILE_VERSION set FILE_VERSION=%WEASEL_VERSION%.%WEASEL_BUILD%
if defined DEVTOOLS_PATH set PATH=%DEVTOOLS_PATH%%PATH%
if not defined BOOST_ROOT echo [FAILED] BOOST_ROOT not set in env.bat & exit /b 1
if not exist "%BOOST_ROOT%\boost" echo [FAILED] Boost not found at %BOOST_ROOT% & exit /b 1
if not exist lib64\rime.lib echo [FAILED] lib64\rime.lib missing, need x64 librime & exit /b 1
if not exist weasel.props cscript //nologo render.js weasel.props BOOST_ROOT PLATFORM_TOOLSET VERSION_MAJOR VERSION_MINOR VERSION_PATCH PRODUCT_VERSION FILE_VERSION
rem If linking fails with LNK1104 on output\WeaselServer.exe, the server is running:
rem run "output\WeaselServer.exe /q" then retry. Not done here to avoid killing the IME.
set BUILD_OPT=/t:Build
if /i "%~1"=="rebuild" set BUILD_OPT=/t:Rebuild
echo Building Win64 ...
msbuild.exe weasel.sln %BUILD_OPT% /p:Configuration=Release /p:Platform=x64 /v:m /nologo /nodeReuse:false /m
if errorlevel 1 goto error
echo.
rem ---- 32-bit companion components ----
rem WeaselServer/WeaselDeployer need a 32-bit rime.lib (not built here);
rem the TSF DLL / IME / Setup do not, so build them for 32-bit apps.
if errorlevel 1 goto error
if errorlevel 1 goto error
if errorlevel 1 goto error

rem 用 solution 目標建置，這樣 $(SolutionDir) 才有效（直接對 vcxproj 建置會丟失 include 路徑）
msbuild.exe weasel.sln /t:WeaselTSF /p:Configuration=Release /p:Platform=Win32 /v:m /nologo /nodeReuse:false
if errorlevel 1 goto error
msbuild.exe weasel.sln /t:WeaselIME /p:Configuration=Release /p:Platform=Win32 /v:m /nologo /nodeReuse:false
if errorlevel 1 goto error
msbuild.exe weasel.sln /t:WeaselSetup /p:Configuration=Release /p:Platform=Win32 /v:m /nologo /nodeReuse:false
if errorlevel 1 goto error

echo [OK] Win64 build finished:
dir /b output\WeaselServer.exe output\WeaselDeployer.exe output\weaselx64.dll output\weaselx64.ime 2>nul
exit /b 0
:error
echo.
echo [FAILED] Win64 build failed.
exit /b 1
