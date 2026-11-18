@echo off
rem Builds sf_http.dll and the offline tests. Requires MSVC x64 (cl.exe) and the StormFabric SDK.
rem Usage: build.bat ["<StormFabric>\include"]   (default: ..\StormFabric\include, or %STORMFABRIC_SDK%)
setlocal
set "SDK=%~1"
if "%SDK%"=="" if defined STORMFABRIC_SDK set "SDK=%STORMFABRIC_SDK%"
if "%SDK%"=="" set "SDK=%~dp0..\StormFabric\include"
for %%I in ("%SDK%") do set "SDK=%%~fI"
if not exist "%SDK%\sw_mod_sdk.h" (
  echo ERROR: sw_mod_sdk.h not found in "%SDK%".
  echo Clone https://github.com/0k0k4/StormFabric and pass its include folder:
  echo   build.bat "C:\path\to\StormFabric\include"
  exit /b 1
)

if not defined VCVARS (
  if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" (
    set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
  ) else if exist "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" (
    set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
  ) else if exist "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" (
    set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
  )
)
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not defined VCVARS if exist "%VSWHERE%" (
  for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do (
    if exist "%%i\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
  )
)
if not defined VCVARS (
  echo ERROR: vcvars64.bat not found. Set VCVARS.
  exit /b 1
)
call "%VCVARS%" >nul || exit /b 1
cd /d "%~dp0"
if not exist build mkdir build

set CFLAGS=/nologo /O2 /MT /EHsc /std:c++17 /W4 /WX /DUNICODE /D_UNICODE /I"%SDK%"

for %%d in (obj_dll obj_selftest obj_modtest) do if not exist build\%%d mkdir build\%%d

set SRC=src\http_client.cpp src\http_registry.cpp src\http_json.cpp

rem The mod itself. Mod DLLs are prefixed (sf_) so they never take the name of a System32 DLL.
cl %CFLAGS% /LD /Fo:build\obj_dll\ /Fe:build\sf_http.dll src\http_mod.cpp %SRC% ^
   /link /DLL /MANIFEST:NO /DEBUG:NONE winhttp.lib || exit /b 1

rem Offline tests (start tests\test_server.py before running them)
cl %CFLAGS% /Fo:build\obj_selftest\ /Fe:build\http_selftest.exe tests\http_selftest.cpp src\http_client.cpp ^
   /link /DEBUG:NONE winhttp.lib || exit /b 1
cl %CFLAGS% /Fo:build\obj_modtest\ /Fe:build\mod_test.exe tests\mod_test.cpp src\http_mod.cpp %SRC% ^
   /link /DEBUG:NONE winhttp.lib || exit /b 1

echo OK
