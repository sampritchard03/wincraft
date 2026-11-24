@echo off
rem Builds the native extension into gdextension\bin\ with MSVC.
rem Usage: build.bat [template_debug^|template_release]
setlocal
cd /d "%~dp0"

set "TARGET=%~1"
if "%TARGET%"=="" set "TARGET=template_debug"

rem Fetch godot-cpp if it isn't there yet.
if not exist "godot-cpp\CMakeLists.txt" (
    echo Cloning godot-cpp...
    git clone --depth 1 --branch 4.5 https://github.com/godotengine/godot-cpp.git godot-cpp || goto :fail
)

rem Set up the MSVC x64 environment (also puts VS's cmake and ninja on PATH).
where cl >nul 2>nul
if not errorlevel 1 goto :have_env
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
    echo Visual Studio C++ build tools not found.
    goto :fail
)
if not exist "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" (
    echo Could not find vcvars64.bat under "%VSDIR%".
    goto :fail
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || goto :fail
:have_env

rem Windows won't let the linker overwrite a DLL Godot has loaded, but it
rem can be renamed: move it aside (restart Godot afterwards to load the new one).
set "DLL=bin\libwincraft_native.windows.%TARGET%.x86_64.dll"
if exist "%DLL%.old" del "%DLL%.old" >nul 2>nul
if exist "%DLL%" ren "%DLL%" "libwincraft_native.windows.%TARGET%.x86_64.dll.old" >nul 2>nul

set "DIR=build\windows-msvc-%TARGET%"
cmake -S . -B "%DIR%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DGODOTCPP_TARGET=%TARGET% || goto :fail
cmake --build "%DIR%" || goto :fail

echo.
echo Built gdextension\bin\libwincraft_native.windows.%TARGET%.x86_64.dll
exit /b 0

:fail
echo.
echo Build failed.
exit /b 1
