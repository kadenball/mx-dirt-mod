# Builds both plugins and assembles the release zip in build\.
#   .\package.ps1 -Version 1.0.0
param([Parameter(Mandatory)][string]$Version,[string]$Zig='')
$ErrorActionPreference='Stop'
$R=$PSScriptRoot; $B="$R\build"
$zigArg=@{}; if($Zig){ $zigArg.Zig=$Zig }
& "$R\build.ps1" @zigArg; if(-not $?){ throw 'core build failed' }
& "$R\build_render.ps1" @zigArg; if(-not $?){ throw 'render build failed' }

$name="MX-Dirt-Mod-$Version"; $S="$B\$name"
if(Test-Path $S){ Remove-Item $S -Recurse -Force }
New-Item -ItemType Directory -Force "$S\plugins","$S\install","$S\launcher" | Out-Null
Copy-Item "$B\mx_dirt.dll" "$S\plugins\mx_dirt.dlo"
Copy-Item "$B\mx_dirt_render.dll" "$S\plugins\mx_dirt_render.dlo"
Copy-Item "$R\src\core\mx_dirt.ini","$R\src\render\mx_dirt_render.ini" "$S\plugins\"
Copy-Item "$R\install\Install.ps1","$R\install\Uninstall.ps1" "$S\install\"
Copy-Item "$R\install\Install.cmd","$R\install\Uninstall.cmd" "$S\"
Copy-Item "$R\launcher\*" "$S\launcher\"
Copy-Item "$R\LICENSE","$R\SUPPORTED_EXE.txt" "$S\"
Copy-Item "$R\README.md" "$S\README.txt"
$zip="$B\$name.zip"; if(Test-Path $zip){ Remove-Item $zip -Force }
Compress-Archive -Path "$S\*" -DestinationPath $zip
Get-ChildItem "$S\plugins" | ForEach-Object { '{0}  {1}' -f (Get-FileHash $_.FullName).Hash.ToLower(),$_.Name } | Set-Content -Encoding ascii "$B\$name.sha256.txt"
"$zip"; Get-Content "$B\$name.sha256.txt"
