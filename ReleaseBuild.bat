@echo off
setlocal

set SOLUTION_DIR=%~dp0
set PROJECT_DIR=%SOLUTION_DIR%KraftonEngine
set BUILD_OUTPUT=%PROJECT_DIR%\Bin\Release
set RELEASE_DIR=%SOLUTION_DIR%ReleaseBuild

REM Load VS Developer environment (for msbuild PATH)
set VSWHERE="%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`%VSWHERE% -latest -property installationPath`) do set VS_PATH=%%i
if not defined VS_PATH (
    echo Visual Studio installation not found.
    pause
    exit /b 1
)
call "%VS_PATH%\Common7\Tools\VsDevCmd.bat" -no_logo

echo ============================================
echo  Release Build Script
echo ============================================

REM 1) Build solution
echo.
echo [1/3] Building Release x64...
msbuild "%SOLUTION_DIR%KraftonEngine.sln" /p:Configuration=Release /p:Platform=x64 /m /v:minimal
if %ERRORLEVEL% neq 0 (
    echo BUILD FAILED
    pause
    exit /b 1
)

REM 2) Prepare output directory
echo.
echo [2/3] Preparing output directory...
if exist "%RELEASE_DIR%" rmdir /s /q "%RELEASE_DIR%"
mkdir "%RELEASE_DIR%"

REM 3) Copy runtime files
echo.
echo [3/3] Copying files...

REM Executable
copy "%BUILD_OUTPUT%\KraftonEngine.exe" "%RELEASE_DIR%\" >nul

REM Shaders / Assets / Settings / Data
xcopy "%PROJECT_DIR%\Shaders" "%RELEASE_DIR%\Shaders\" /e /i /q /y >nul
xcopy "%PROJECT_DIR%\Asset" "%RELEASE_DIR%\Asset\" /e /i /q /y >nul
xcopy "%PROJECT_DIR%\Settings" "%RELEASE_DIR%\Settings\" /e /i /q /y >nul
xcopy "%PROJECT_DIR%\Data" "%RELEASE_DIR%\Data\" /e /i /q /y >nul

REM Optional: copy precompiled shader cache (CSO)
echo.
echo [Optional] Copying ShaderCache...
set SHADER_CACHE_SRC1=%PROJECT_DIR%\Saves\ShaderCache
set SHADER_CACHE_SRC2=%SOLUTION_DIR%\Saves\ShaderCache
set SHADER_CACHE_DST=%RELEASE_DIR%\Saves\ShaderCache

if exist "%SHADER_CACHE_SRC1%" (
    mkdir "%RELEASE_DIR%\Saves" >nul 2>&1
    xcopy "%SHADER_CACHE_SRC1%" "%SHADER_CACHE_DST%\" /e /i /q /y >nul
    echo   Copied from: %SHADER_CACHE_SRC1%
) else if exist "%SHADER_CACHE_SRC2%" (
    mkdir "%RELEASE_DIR%\Saves" >nul 2>&1
    xcopy "%SHADER_CACHE_SRC2%" "%SHADER_CACHE_DST%\" /e /i /q /y >nul
    echo   Copied from: %SHADER_CACHE_SRC2%
) else (
    echo   ShaderCache not found. Skipping.
)

echo.
echo ============================================
echo  Build complete: %RELEASE_DIR%
echo ============================================
echo.
echo  ReleaseBuild/
echo    KraftonEngine.exe
echo    Shaders/
echo    Asset/
echo    Data/
echo    Settings/
echo    Saves/ShaderCache/ (if present)
echo.
pause
