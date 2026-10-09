@echo off
rem Builds gw2patch and gw2emu as static binaries into bin\ (MinGW-w64 g++ on PATH).
setlocal
cd /d "%~dp0"
if not exist bin mkdir bin
g++ -std=c++17 -O2 -Wall -static -static-libgcc -static-libstdc++ -Icommon patcher\main.cpp -o bin\gw2patch.exe || exit /b 1
g++ -std=c++17 -O2 -Wall -static -static-libgcc -static-libstdc++ -Icommon emu\main.cpp emu\msgconn.cpp emu\protocol.cpp -o bin\gw2emu.exe -lws2_32 || exit /b 1
echo built bin\gw2patch.exe bin\gw2emu.exe
