# Microsoft Developer Studio Project File - Name="cassette" - Package Owner=<4>
# Microsoft Developer Studio Generated Build File, Format Version 6.00
# ** DO NOT EDIT **

# TARGTYPE "Win32 (x86) Console Application" 0x0103

CFG=cassette - Win32
!MESSAGE This is not a valid makefile. To build this project using NMAKE,
!MESSAGE use the Export Makefile command and run
!MESSAGE 
!MESSAGE NMAKE /f "cassette.mak".
!MESSAGE 
!MESSAGE You can specify a configuration when running NMAKE
!MESSAGE by defining the macro CFG on the command line. For example:
!MESSAGE 
!MESSAGE NMAKE /f "cassette.mak" CFG="cassette - Win32"
!MESSAGE 
!MESSAGE Possible choices for configuration are:
!MESSAGE 
!MESSAGE "cassette - Win32" (based on "Win32 (x86) Console Application")
!MESSAGE 

# Begin Project
# PROP AllowPerConfigDependencies 0
CPP=cl.exe
MTL=midl.exe

!IF "$(CFG)" == "cassette - Win32"

# PROP BASE Use_MFC 0
# PROP BASE Use_Debug_Libraries 0
# PROP BASE Output_Dir "Release"
# PROP BASE Intermediate_Dir "Release"
# PROP BASE Target_Dir ""
# PROP Use_MFC 0
# PROP Use_Debug_Libraries 0
# PROP Output_Dir "Release"
# PROP Intermediate_Dir "Release"
# PROP Ignore_Export_Lib 0
# PROP Target_Dir ""
# ADD BASE CPP /nologo /W3 /I "include" /TC /MT /G6 /O2
# ADD CPP /nologo /W3 /I "include" /TC /MT /G6 /O2
# ADD BASE MTL /nologo /win32
# ADD MTL /nologo /win32
BSC32=bscmake.exe
# ADD BASE BSC32 /nologo
# ADD BSC32 /nologo
LINK32=link.exe
# ADD BASE LINK32 /nologo /subsystem:console
# ADD LINK32 /nologo /subsystem:console
# Begin Custom Build - Publishing to target directory (..\)...
TargetDir=.\Release
TargetPath=.\Release\cassette.exe
InputPath=.\Release\cassette.exe
SOURCE="$(InputPath)"
BuildCmds= \
	if exist ..\cassette.exe attrib -r ..\cassette.exe \
	move /y $(TargetPath) ..\cassette.exe > nul \
	del /f /s /q .\*.exe .\*.obj > nul \

"..\cassette.exe" : $(SOURCE) "$(INTDIR)" "$(OUTDIR)"
$(BuildCmds)
	
# End Custom Build

!ENDIF

# Begin Target

# Name "cassette - Win32"

# Begin Group "Source Files"
# PROP Default_Filter "cpp;c;cxx;rc;def;r;odl;idl;hpj;bat"

# Begin Source File
SOURCE=.\main.c
# End Source File
# Begin Source File
SOURCE=.\ini.c
# End Source File
# Begin Source File
SOURCE=.\crc16.c
# End Source File
# Begin Source File
SOURCE=.\frame.c
# End Source File
# Begin Source File
SOURCE=.\modulation.c
# End Source File
# Begin Source File
SOURCE=.\pcm_io.c
# End Source File
# Begin Source File
SOURCE=.\pcm_wav.c
# End Source File

# Begin Source File
SOURCE=.\tx.c
# End Source File
# Begin Source File
SOURCE=.\rx.c
# End Source File
# Begin Source File
SOURCE=.\analog.c
# End Source File
# Begin Source File
SOURCE=.\dsp_host.c
# End Source File

# Begin Source File
SOURCE=.\metadata.c
# End Source File
# Begin Source File
SOURCE=.\control.c
# End Source File
# Begin Source File
SOURCE=.\fdkaac_dll.c
# End Source File
# Begin Source File
SOURCE=.\faad2_dll.c
# End Source File

# End Group

# End Target
# End Project
