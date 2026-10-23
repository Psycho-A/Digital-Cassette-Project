@echo off

if "%~1"=="" set test_file=test_radio
if not "%~1"=="" set test_file=%~nx1
del /f /q "%test_file%.wav*" 2> nul

call curl -sfkL "http://ais-sa3.cdnstream1.com/2606_128.aac" | ^
cassette.exe tx - - -input_backend=wav -output_backend=wav   -stream_class=10 -frame_capacity=768 | ^
cassette.exe rx - - -input_backend=wav -output_backend=winmm -stream_class=10 -frame_capacity=768 2> nul

pause
