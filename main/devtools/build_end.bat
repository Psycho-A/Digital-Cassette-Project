@rem
@rem If we're the top-level process and there were errors,
@rem issue a warning
@rem
echo.
if "%build_level%"=="2" goto done
if "%build_level%"=="3" goto done
if "%BUILD_ERROR%"==""  goto build_ok

echo *********************
echo *********************
echo *** Build Errors! ***
echo *********************
echo *********************
goto done

:build_ok
echo ================
echo Build succeeded!
echo ================
echo.

:done
@rem clear VS registry for portable usage
if exist   "%~dp0..\..\vc_studio\clearregistry.reg" (
regedit /s "%~dp0..\..\vc_studio\clearregistry.reg" )
@rem All done.  Decrement build_level
if "%build_level%"=="1" set build_level=
if "%build_level%"=="2" set build_level=1
if "%build_level%"=="3" set build_level=2
if "%BUILD_ERROR%"=="" (
if not "%1"=="-nopause" (
	pause> nul)
) else (
	pause> nul
	exit
)
