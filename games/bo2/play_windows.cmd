@echo off
rem Double-click to play the PC build: native renderer, god mode on (test builds), normal volume, in a window
rem (not fullscreen, so it can be captured for streaming). Frames are presented in step with the display (FIFO):
rem the faster present modes skip the desktop compositor while the window has focus, and window capture (Discord)
rem then shows a frozen frame.
rem It goes through games/bo2/run_windows.sh, which waits if another copy of the game is running and writes the log to
rem games/bo2/logs. Game data: the data folder tools/data_dir.sh finds. Close this window or the game to stop.
set "BASH=%ProgramFiles%\Git\bin\bash.exe"
if not exist "%BASH%" (
  echo Git Bash was not found at "%BASH%".
  pause
  exit /b 1
)
cd /d "%~dp0..\.."
set "BO2_VOLUME=100"
set "BO2_FOREGROUND=1"
"%BASH%" games/bo2/run_windows.sh 0 --bo2zm_renderer=native --bo2zm_test_god=true --fullscreen=false --vulkan_allow_present_mode_immediate=false --vulkan_allow_present_mode_mailbox=false %*
