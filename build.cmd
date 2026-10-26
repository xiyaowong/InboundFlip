@echo off
rem InboundFlip build script (MinGW-w64, e.g. from MSYS2 or w64devkit)
setlocal
cd /d "%~dp0"

if not exist build md build
windres resource.rc -O coff -o build\resource.o || goto :err

gcc -O2 -s -municode -mwindows -Wall -Wextra ^
    main.c build\resource.o ^
    -o InboundFlip.exe -ladvapi32 || goto :err

echo built: InboundFlip.exe
for %%F in (InboundFlip.exe) do echo size:  %%~zF bytes

rem optional compression (needs upx in PATH):
if not "%1"=="--upx" goto :done
upx --best --lzma InboundFlip.exe
for %%F in (InboundFlip.exe) do echo size after UPX:  %%~zF bytes
goto :done

:err
echo build FAILED
exit /b 1

:done
endlocal
