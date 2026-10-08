@echo off
rem Builds the test bot (tools\bot.exe).
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1
cd /d "%~dp0.."
if not exist build\bot mkdir build\bot
cl /nologo /O2 /EHsc /std:c++17 /MT /W3 /D_CRT_SECURE_NO_WARNINGS /DWIN32_LEAN_AND_MEAN /DNOMINMAX /wd5287 /wd4101 ^
  /Ienet\include /Fobuild\bot\ /Fe:tools\bot.exe ^
  tools\bot.cpp enet\callbacks.c enet\compress.c enet\host.c enet\list.c enet\packet.c enet\peer.c enet\protocol.c enet\win32.c enet\address.c ^
  ws2_32.lib winmm.lib
exit /b %errorlevel%
