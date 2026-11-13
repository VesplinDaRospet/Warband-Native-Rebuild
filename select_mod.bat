@echo off
rem Rebuilt game root - text-mode mod selector (GitHub entry)
rem Usage: drop mod folders into Modules\, run this script, pick a number.
setlocal enabledelayedexpansion
cd /d "%~dp0"
echo ============================================
echo   Rebuilt game root - select mod
echo ============================================
set /a n=0
for /d %%d in (Modules\*) do (
  if exist "%%d\map.txt" (
    set /a n+=1
    echo   !n!. %%~nxd
    set "mod!n!=%%~nxd"
  )
)
if %n%==0 (
  echo No usable mod found in Modules\ (a mod needs map.txt)
  pause
  exit /b 1
)
set /p pick=Enter number:
set "m="
if defined mod%pick% set "m=!mod%pick%!"
if not defined m (
  echo Invalid number
  pause
  exit /b 1
)
set "root=%CD:\=\\%"
> engine_config.json echo {"game_root":"%root%","default_module":"%m%","window_w":1280,"window_h":720}
echo Launching %m% ...
start "" vk_map.exe
endlocal
