@echo off
REM ---------------------------------------------------------------------------
REM  Ember in QEMU.  Boots build\ember.img exactly as a machine would boot the
REM  USB stick, with sound and a mouse attached.
REM
REM      start.bat              the desktop, in a window
REM      start.bat big          the same, scaled up for recording
REM      start.bat full         full screen  (Ctrl+Alt+F to come back)
REM      start.bat build        rebuild the image first, then run
REM
REM  Inside: type EMBER for the desktop, WIN for the old shell, HELP for the
REM  command list.  Ctrl+Alt+G releases the mouse back to Windows.
REM ---------------------------------------------------------------------------
setlocal

set "QEMU=C:\Program Files\qemu\qemu-system-i386.exe"
if not exist "%QEMU%" set "QEMU=qemu-system-i386"

set "IMG=%~dp0build\ember.img"

if /i "%~1"=="build" (
    echo Building...
    python "%~dp0build.py" --size 64 || goto :error
    shift
)

if not exist "%IMG%" (
    echo No image yet: building one.
    python "%~dp0build.py" --size 64 || goto :error
)

REM The guest draws at up to 1280x1024, so give the card enough memory for it.
set "DISPLAY=-display gtk,zoom-to-fit=on"
if /i "%~1"=="big"  set "DISPLAY=-display gtk,zoom-to-fit=on,window-close=on"
if /i "%~1"=="full" set "DISPLAY=-display gtk,zoom-to-fit=on -full-screen"

"%QEMU%" ^
  -m 64 ^
  -drive file="%IMG%",format=raw,if=ide ^
  -vga std -global VGA.vgamem_mb=16 ^
  -device intel-hda -device hda-duplex ^
  -rtc base=localtime ^
  -name "Ember" ^
  %DISPLAY%

goto :eof

:error
echo.
echo Build failed.
exit /b 1
