@echo off

if "%~1"=="" set test_file=test_file_img.bmp
if not "%~1"=="" set test_file=%~nx1
del /f /q "%test_file%.*.bmp*" 2> nul

call cassette.exe rx "%test_file%.wav" "%test_file%.wav.bmp" -input_backend=wav -output_backend=wav
if exist "%test_file%.wav.bmp" start "" "%test_file%.wav.bmp"

pause
