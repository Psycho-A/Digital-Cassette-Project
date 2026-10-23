@echo off

if "%~1"=="" set test_file=cassette.exe
if not "%~1"=="" set test_file=%~nx1
del /f /q "%test_file%.bin*" 2> nul

cassette.exe tx "%test_file%" - -input_backend=wav -output_backend=wav | ^
cassette.exe rx - "%test_file%.wav.bin" -input_backend=wav -output_backend=wav

pause
