@echo off

if "%~1"=="" set test_file=test_record.ask
if not "%~1"=="" set test_file=%~nx1
del /f /q "%test_file%.wav*" 2> nul

call cassette.exe tx - - -input_backend=winmm -output_backend=winmm

pause
