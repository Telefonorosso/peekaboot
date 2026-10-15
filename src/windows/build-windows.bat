@echo off
setlocal
cl /nologo /O2 /W4 rdisk_host_winusb-ERRORS-TELEMETRY.c /Fe:rdisk_host_winusb.exe /link winusb.lib setupapi.lib user32.lib
if errorlevel 1 exit /b 1
cl /nologo /O2 /W4 l2boot-gui-THROUGHPUT-ERRORS-NO-RAW.c /Fe:l2boot-gui.exe /link /SUBSYSTEM:WINDOWS comdlg32.lib user32.lib gdi32.lib shell32.lib
if errorlevel 1 exit /b 1
echo Built rdisk_host_winusb.exe and l2boot-gui.exe
endlocal
