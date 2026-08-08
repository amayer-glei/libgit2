@echo off
rem Build gut.exe using the CMake and MSVC bundled with Visual Studio.
setlocal
set "SCRIPT_DIR=%~dp0"
if "%SCRIPT_DIR:~-1%"=="\" set "SCRIPT_DIR=%SCRIPT_DIR:~0,-1%"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"

if exist "%VSWHERE%" goto have_vswhere
echo error: vswhere not found at "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" 1>&2
exit /b 1
:have_vswhere

for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VS_DIR=%%i"
if defined VS_DIR goto have_vs
echo error: no Visual Studio installation found 1>&2
exit /b 1
:have_vs

set "CMAKE=%VS_DIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if exist "%CMAKE%" goto have_cmake
echo error: bundled cmake not found under "%VS_DIR%" 1>&2
exit /b 1
:have_cmake

"%CMAKE%" -S "%SCRIPT_DIR%" -B "%SCRIPT_DIR%\build"
if errorlevel 1 exit /b 1

"%CMAKE%" --build "%SCRIPT_DIR%\build" --config Release --target gut
if errorlevel 1 exit /b 1

set "GUT_EXE=%SCRIPT_DIR%\build\Release\gut.exe"
if not exist "%GUT_EXE%" for /r "%SCRIPT_DIR%\build" %%f in (gut.exe) do set "GUT_EXE=%%f"
echo.
echo Built: %GUT_EXE%
