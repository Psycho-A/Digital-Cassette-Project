@echo off
pushd "%~dp0"

if "%~1"=="" set test_file=cassette.exe
if not "%~1"=="" set "test_file=%~1"
del /f /q "%test_file%.wav" 2> nul

call cassette.exe tx "%test_file%" "%test_file%.wav" -input_backend=wav

pause
