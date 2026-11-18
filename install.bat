@echo off
rem Installs sf_http into <game>\mods\http and enables it, and copies the example addon to
rem %APPDATA%\Stormworks\data\missions\sf_http_test.
rem Usage: install.bat "<Stormworks folder>"   (or set STORMWORKS_DIR). The game must be closed.
setlocal
set "GAME=%~1"
if "%GAME%"=="" if defined STORMWORKS_DIR set "GAME=%STORMWORKS_DIR%"
if "%GAME%"=="" (
  echo Usage: install.bat "C:\path\to\Stormworks"
  exit /b 1
)
if not exist "%GAME%\stormworks64.exe" (
  echo stormworks64.exe not found in "%GAME%"
  exit /b 1
)
if not exist "%GAME%\iphlpapi.dll" echo WARNING: the StormFabric loader (iphlpapi.dll) is not installed in "%GAME%".
tasklist /FI "IMAGENAME eq stormworks64.exe" | find /I "stormworks64.exe" >nul && (
  echo Stormworks is running: close it first.
  exit /b 1
)
set "DLL=%~dp0build\sf_http.dll"
if not exist "%DLL%" set "DLL=%~dp0sf_http.dll"
if not exist "%DLL%" (
  echo sf_http.dll missing: run build.bat first
  exit /b 1
)
set "DEST=%GAME%\mods\http"
if not exist "%DEST%" mkdir "%DEST%"
copy /Y "%~dp0mod.json" "%DEST%\mod.json" >nul || exit /b 1
copy /Y "%DLL%" "%DEST%\sf_http.dll" >nul || exit /b 1

set "INI=%GAME%\mods\_modloader.ini"
rem "(echo ...)>>" keeps cmd from reading "=1>>" as a redirection of handle 1.
findstr /B /C:"http=" "%INI%" >nul 2>&1 || ((echo http=1)>>"%INI%")
findstr /B /C:"http=1" "%INI%" >nul 2>&1 || echo NOTE: http is disabled in "%INI%" - enable it in the MODS menu.

set "ADDON=%APPDATA%\Stormworks\data\missions\sf_http_test"
if exist "%APPDATA%\Stormworks\data\missions" (
  if not exist "%ADDON%" mkdir "%ADDON%"
  copy /Y "%~dp0examples\playlist.xml" "%ADDON%\playlist.xml" >nul
  rem Keep an installed script.lua that already holds a Discord webhook URL.
  findstr /R /C:"DISCORD_WEBHOOK = .https" "%ADDON%\script.lua" >nul 2>&1 && (
    echo script.lua kept: it contains your DISCORD_WEBHOOK. Copy examples\script.lua by hand to update it.
  ) || copy /Y "%~dp0examples\script.lua" "%ADDON%\script.lua" >nul
  echo Example addon: "%ADDON%"
)

echo Installed: "%DEST%"
echo Log: "%GAME%\StormworksModLoader.log"
echo Config (created on first launch): "%DEST%\config\http.cfg.bin"
