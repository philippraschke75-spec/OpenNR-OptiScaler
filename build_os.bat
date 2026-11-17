@echo off
rem OptiScaler (fg-provider worktree) + person_worker (its project file asks for v145; VS2022 has v143)
call E:\VS2022BuildTools\VC\Auxiliary\Build\vcvars64.bat
cd /d E:\opennr-fork-fg
msbuild OptiScaler\OptiScaler.vcxproj /m /p:Configuration=Release /p:Platform=x64 /p:SolutionDir=E:\opennr-fork-fg\ /v:minimal > E:\opennr-fork-fg\build_os.log 2>&1
set RC=%ERRORLEVEL%
msbuild OptiScaler\dlssnr\person\worker\person_worker.vcxproj /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v143 /p:SolutionDir=E:\opennr-fork-fg\ /v:minimal >> E:\opennr-fork-fg\build_os.log 2>&1
if %RC%==0 set RC=%ERRORLEVEL%
echo EXIT %RC% >> E:\opennr-fork-fg\build_os.log
exit /b %RC%
