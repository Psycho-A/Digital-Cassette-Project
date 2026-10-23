@echo off

if "%~1"=="" set test_file=cassette.exe
if not "%~1"=="" set test_file=%~nx1
del /f /q "%test_file%.wav" 2> nul

call busybox cat %test_file% | ^
cassette.exe tx - "%test_file%-stdin.wav" -input_backend=wav -stream_class=0

pause
