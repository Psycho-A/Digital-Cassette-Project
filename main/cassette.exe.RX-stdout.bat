@echo off

if "%~1"=="" set test_file=cassette.exe.wav
if not "%~1"=="" set test_file=%~nx1
del /f /q "%test_file%.bin*" 2> nul

call cassette.exe rx "%test_file%" - -output_backend=wav > %test_file%.bin

pause
