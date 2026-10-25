@echo off
rem Builds the libraries needed for voice support (Opus, MLS, DAVE) with Visual Studio.
rem
rem Usage:  build-voice-deps.cmd [x64|x86] [path to the OpenSSL you build Discord Messenger with]
rem
rem Run it from a "Developer Command Prompt for VS", or anywhere cmake is on the PATH.
rem Results end up in build-voice\<platform>\lib\Debug and \Release, which is where
rem vs\Voice.props looks.  Then build the Visual Studio project with /p:EnableVoice=true.

setlocal
set PLATFORM=%1
if "%PLATFORM%"=="" set PLATFORM=x64
set OPENSSL=%2
if "%OPENSSL%"=="" set OPENSSL=%OPENSSL_INSTALL%
if "%PLATFORM%"=="x86" (set ARCH=Win32) else (set ARCH=%PLATFORM%)

if "%OPENSSL%"=="" (
	echo Please say where OpenSSL is: build-voice-deps.cmd %PLATFORM% ^<openssl dir^>
	exit /b 1
)

set SRC=%~dp0voice-deps
set OUT=%~dp0build-voice\%PLATFORM%

cmake -S "%SRC%" -B "%OUT%" -A %ARCH% -DOPENSSL_ROOT_DIR="%OPENSSL%" || exit /b 1
cmake --build "%OUT%" --config Release --target voice_deps || exit /b 1
cmake --build "%OUT%" --config Debug --target voice_deps || exit /b 1

echo.
echo Done.  Build the Visual Studio project with /p:EnableVoice=true
