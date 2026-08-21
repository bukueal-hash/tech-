$msbuild = "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe"
$project = "f:\Test\ARCs\ArcRaiders.sln"
$logFile = "f:\Test\ARCs\build_detailed.log"
& $msbuild $project /p:Configuration=Release /p:Platform=x64 /t:Rebuild /v:detailed 2>&1 | Tee-Object -FilePath $logFile