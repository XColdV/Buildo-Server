@echo off
rem Builds buildo-server.exe with the MSVC Build Tools (x86, like the 2012 client).
setlocal
set VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars32.bat
if not exist "%VCVARS%" (
  echo MSVC Build Tools not found at "%VCVARS%"
  exit /b 1
)
call "%VCVARS%" >nul || exit /b 1
cd /d "%~dp0"
set OUT=%~1
if "%OUT%"=="" set OUT=buildo-server.exe
if not exist build mkdir build
cl /nologo /O2 /EHsc /std:c++17 /MT /W3 /D_CRT_SECURE_NO_WARNINGS /DWIN32_LEAN_AND_MEAN /DNOMINMAX /wd5287 /wd4101 ^
  /Ienet\include /Fobuild\ /Fe:%OUT% ^
  src\*.cpp enet\callbacks.c enet\compress.c enet\host.c enet\list.c enet\packet.c enet\peer.c enet\protocol.c enet\win32.c enet\address.c ^
  ws2_32.lib winmm.lib bcrypt.lib
exit /b %errorlevel%
