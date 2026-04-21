@echo off
REM fw.bat — Pico 2 / Zephyr firmware helper
REM Run from the project root directory.
REM
REM Usage:  fw <command> [options]
REM
REM Override configuration by setting environment variables before calling:
REM   set EXTRA_CONF=debug_minimal_cdc.conf
REM   set BUILD_DIR=build-test
REM   set WEST_TOPDIR=D:\zephyrproject
REM   fw build-fresh

setlocal

REM Route to the appropriate sub-script
if "%1"==""             goto help
if "%1"=="help"         goto help

REM venv / west targets
if "%1"=="venv-info"    call scripts\venv.bat venv-info    & goto end
if "%1"=="venv-create"  call scripts\venv.bat venv-create  & goto end
if "%1"=="venv-install" call scripts\venv.bat venv-install & goto end
if "%1"=="west-update"  call scripts\venv.bat west-update  & goto end
if "%1"=="west-blobs"   call scripts\venv.bat west-blobs   & goto end

REM build targets
if "%1"=="build"             call scripts\build.bat build             & goto end
if "%1"=="build-fresh"       call scripts\build.bat build-fresh       & goto end
if "%1"=="build-wifi-probe"  call scripts\build.bat build-wifi-probe  & goto end
if "%1"=="menuconfig"        call scripts\build.bat menuconfig        & goto end
if "%1"=="clean"             call scripts\build.bat clean             & goto end
if "%1"=="clean-all"         call scripts\build.bat clean-all         & goto end

REM flash targets
if "%1"=="flash"             call scripts\flash.bat flash             & goto end
if "%1"=="flash-uf2"         call scripts\flash.bat flash-uf2         & goto end
if "%1"=="flash-picotool"    call scripts\flash.bat flash-picotool    & goto end
if "%1"=="flash-find-drive"  call scripts\flash.bat flash-find-drive  & goto end

echo Unknown command: %1
echo Run  fw help  for a list of commands.
exit /b 1

REM ─────────────────────────────────────────────────────────────────────────────
:help
echo.
echo Pico 2 firmware - available commands
echo ======================================
echo.
echo   Venv / west workspace
echo     fw venv-info          print resolved venv / west paths
echo     fw venv-create        create .venv in WEST_TOPDIR (if absent)
echo     fw venv-install       install / upgrade west + Zephyr requirements
echo     fw west-update        run west update to sync all modules
echo     fw west-blobs         fetch nRF Wi-Fi binary blobs
echo.
echo   Build
echo     fw build              incremental build (re-uses CMake cache)
echo     fw build-fresh        pristine build (wipes cache)
echo     fw build-wifi-probe   pristine build with debug_wifi_probe.conf
echo     fw menuconfig         open Kconfig menu for current build
echo     fw clean              delete build directory
echo     fw clean-all          delete all build directories
echo.
echo   Flash
echo     fw flash              copy zephyr.uf2 to Pico BOOTSEL drive
echo     fw flash-picotool     flash via picotool
echo     fw flash-find-drive   scan for the Pico BOOTSEL drive
echo.
echo   Configuration (set before calling fw)
echo     WEST_TOPDIR     default: C:\Users\veswaranandam.MILKYWAY\zephyrproject
echo     ZEPHYR_BASE     default: %%WEST_TOPDIR%%\zephyr
echo     BOARD           default: rpi_pico2/rp2350a/m33
echo     SHIELD          default: rpi_pico_uno_flexypin nrf7002ek
echo     BUILD_DIR       default: build
echo     EXTRA_CONF      default: (none)
echo     PICO_DRIVE_LABEL default: RP2350
echo.
echo   Examples
echo     fw build
echo     set EXTRA_CONF=debug_minimal_cdc.conf ^&^& fw build-fresh
echo     fw flash
echo     fw build ^&^& fw flash
echo.

:end
endlocal
