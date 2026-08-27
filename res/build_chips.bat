@echo off
setlocal

:: 1. Ask for the project folder name or sub-path (PROJECT_DIR)
set /p PROJECT_DIR="Enter the name of the project folder (e.g. hack, nes/6502): "

:: Normalize any forward slashes to backslashes
set "PROJECT_DIR=%PROJECT_DIR:/=\%"

:: Define paths based on script location (%~dp0)
set "GLOBAL_DIR=%~dp0global"
set "TARGET_PROJ_DIR=%~dp0projects\%PROJECT_DIR%"

:: Extract the leaf folder name (e.g., gets '6502' from 'nes\6502') for the output file
for %%I in ("%TARGET_PROJ_DIR%") do set "PROJ_NAME=%%~nxI"
set "OUTPUT_FILE=%TARGET_PROJ_DIR%\%PROJ_NAME%-chips.txt"

:: Validate that the project directory exists
:: (Adding a trailing slash ensures it checks for a directory specifically)
if not exist "%TARGET_PROJ_DIR%\" (
    echo Error: Project directory "%TARGET_PROJ_DIR%" does not exist.
    pause
    exit /b 1
)

:: 2. Cleanup old output file if it exists
if exist "%OUTPUT_FILE%" del "%OUTPUT_FILE%"

echo Building %PROJ_NAME%-chips.txt...
set "first=1"

:: 3. Scan the global directory first for .hdl and .const files
if exist "%GLOBAL_DIR%" (
    echo Scanning global directory "%GLOBAL_DIR%"...
    for /r "%GLOBAL_DIR%" %%F in (*.hdl *.const) do call :AppendFile "%%F"
) else (
    echo Global directory not found at "%GLOBAL_DIR%", skipping...
)

:: 4. Scan the project directory for .hdl and .const files
echo Scanning project directory "%TARGET_PROJ_DIR%"...
for /r "%TARGET_PROJ_DIR%" %%F in (*.hdl *.const) do call :AppendFile "%%F"

echo.
echo Done! Code combined into %PROJ_NAME%-chips.txt
pause
exit /b 0

:: ---------------------------------------------------------
:: Subroutine to append files with the separator
:: ---------------------------------------------------------
:AppendFile
if defined first (
    :: If first file, clear the flag and do not print separator
    set "first="
) else (
    :: For subsequent files, print \n---\n
    echo.>>"%OUTPUT_FILE%"
    echo --->>"%OUTPUT_FILE%"
    echo.>>"%OUTPUT_FILE%"
)

:: Attach the comment with the file's name and extension (e.g., // ChipName.hdl)
echo // %~nx1>>"%OUTPUT_FILE%"

:: Append the actual file content
type "%~1">>"%OUTPUT_FILE%"
exit /b