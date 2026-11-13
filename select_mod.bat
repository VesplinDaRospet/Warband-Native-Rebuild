@echo off
rem 骑砍重构本体 · 选 Mod（文本版，GitHub 留存入口）
rem 用法：把 mod 目录原封不动放进 Modules\，双击本脚本选序号启动。
setlocal enabledelayedexpansion
cd /d "%~dp0"
echo ============================================
echo   骑砍重构本体 · 选 Mod
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
  echo Modules 里没有可用 mod（需要含 map.txt 的 mod 目录）
  pause
  exit /b 1
)
set /p pick=输入序号启动：
set "m="
if defined mod%pick% set "m=!mod%pick%!"
if not defined m (
  echo 序号无效
  pause
  exit /b 1
)
set "root=%CD:\=\\%"
> engine_config.json echo {"game_root":"%root%","default_module":"%m%","window_w":1280,"window_h":720}
echo 启动 %m% ...
start "" vk_map.exe
endlocal
