@echo off
rem Build layer_pin.aux2 with MinGW-w64 (x86_64)
cd /d "%~dp0"
g++ -std=c++17 -O2 -shared -municode -s -o layer_pin.aux2 src\layer_pin.cpp -static -static-libgcc -static-libstdc++ -luser32 -lgdi32 -lmsimg32
if errorlevel 1 exit /b 1
echo built layer_pin.aux2
