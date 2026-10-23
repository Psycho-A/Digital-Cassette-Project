@echo off

if "%~1"=="" set test_file=cassette.exe
if not "%~1"=="" set test_file=%~nx1
del /f /q "%test_file%.wav*" 2> nul

call cassette.exe tx "%test_file%" - -output_backend=wav > %test_file%.raw

pause
