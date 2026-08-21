$msbuild = "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe"
$project = "f:\Test\ARCs\ArcRaiders.sln"
& $msbuild $project /p:Configuration=Release /p:Platform=x64 /t:Rebuild /v:minimal