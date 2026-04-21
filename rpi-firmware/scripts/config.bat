@echo off
REM scripts\config.bat — shared configuration
REM Called via  `call "%~dp0config.bat"`  at the top of every other script.
REM Override any variable by setting it in your environment before calling fw.bat.

REM ── West workspace ────────────────────────────────────────────────────────────
if not defined WEST_TOPDIR set WEST_TOPDIR=C:\Users\veswaranandam.MILKYWAY\zephyrproject
if not defined ZEPHYR_BASE  set ZEPHYR_BASE=%WEST_TOPDIR%\zephyr
if not defined VENV_DIR     set VENV_DIR=%WEST_TOPDIR%\.venv

set PYTHON=%VENV_DIR%\Scripts\python.exe
set WEST_CMD="%PYTHON%" -m west

REM ── Board / build config ──────────────────────────────────────────────────────
if not defined BOARD       set BOARD=rpi_pico2/rp2350a/m33
if not defined SHIELD      set SHIELD=nrf7002_pico2
if not defined BUILD_DIR   set BUILD_DIR=build
if not defined EXTRA_CONF  set EXTRA_CONF=
if not defined BOARD_ROOT  set BOARD_ROOT=%CD%

REM Overlay path — CMake needs forward slashes on Windows
set _OV=%CD%\pico-usbip.overlay
set OVERLAY_FILE=%_OV:\=/%
set BOARD_ROOT_CMAKE=%BOARD_ROOT:\=/%

REM ── Flash ─────────────────────────────────────────────────────────────────────
if not defined PICO_DRIVE_LABEL set PICO_DRIVE_LABEL=RP2350
if not defined PICOTOOL         set PICOTOOL=picotool
