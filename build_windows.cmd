@echo off
setlocal

if not defined VSCMD_VER (
    if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" (
        call "%ProgramFiles%\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64
        if errorlevel 1 exit /b %errorlevel%
    ) else (
        echo Run this script from a Visual Studio 2022 x64 Developer Command Prompt.
        exit /b 1
    )
)

set "DIST_DIR=%~dp0dist"
if not exist "%DIST_DIR%" mkdir "%DIST_DIR%"

if /I "%~1"=="--no-boost" goto BUILD_NO_BOOST

set "BOOST_INCLUDE=%~1"
if not defined BOOST_INCLUDE set "BOOST_INCLUDE=%BOOST_ROOT%"
if not exist "%BOOST_INCLUDE%\boost\unordered\unordered_flat_map.hpp" (
    echo Pass the Boost include directory as the first argument or set BOOST_ROOT.
    exit /b 1
)

cl.exe /nologo /std:c++20 /EHsc /O2 /MT /DNDEBUG /utf-8 /DBOOST_ALL_NO_LIB /I"%BOOST_INCLUDE%" "%~dp0Edax find book error tool0_8boost.cpp" /Fo"%DIST_DIR%\Edax_find_book_error_tool_0_8boost.obj" /Fe"%DIST_DIR%\Edax_find_book_error_tool_0_8boost.exe"
if errorlevel 1 exit /b %errorlevel%

:BUILD_NO_BOOST
cl.exe /nologo /std:c++20 /EHsc /O2 /MT /DNDEBUG /utf-8 "%~dp0Edax find book error tool0_8.cpp" /Fo"%DIST_DIR%\Edax_find_book_error_tool_0_8.obj" /Fe"%DIST_DIR%\Edax_find_book_error_tool_0_8.exe"
if errorlevel 1 exit /b %errorlevel%

echo Built requested x64 executable(s) in "%DIST_DIR%".
exit /b 0
