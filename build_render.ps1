# Builds the render plugin: build\mx_dirt_render.dll (install as plugins\mx_dirt_render.dlo).
#   .\build_render.ps1                 # zig from PATH, else the bundled zig next to this repo
#   .\build_render.ps1 -Zig C:\path\zig.exe
param([string]$Zig='')
$ErrorActionPreference='Stop'
(Get-Process -Id $PID).PriorityClass='BelowNormal'
$R=$PSScriptRoot; $B=Join-Path $R 'build'; $S=Join-Path $R 'src\render'

if(-not $Zig){
    $onPath=Get-Command zig -ErrorAction SilentlyContinue
    if($onPath){ $Zig=$onPath.Source }
    else{ $Zig=Join-Path $R '..\..\work\build-libs\ziglang\zig.exe' }
}
if(-not (Test-Path $Zig)){ throw "zig not found: $Zig (pass -Zig <path to zig.exe>)" }

New-Item -ItemType Directory -Force $B | Out-Null
$env:ZIG_LOCAL_CACHE_DIR=Join-Path $B 'zig-cache-render'
$env:ZIG_GLOBAL_CACHE_DIR=Join-Path $B 'zig-global-cache'

$units=@(
    'mxdirt_render_host.c',
    'mxdirt_render_content.c',
    'mxdirt_terrain_scope.c',
    'mxdirt_dirt.c',
    'mxdirt_dirt_gl.c'
) | ForEach-Object { Join-Path $S $_ }
$out=Join-Path $B 'mx_dirt_render.dll'
$cc=@('cc','-shared','-target','x86_64-windows-gnu','-std=c11','-O2','-msse2',
    '-Wall','-Wextra','-Werror','-ffp-contract=off','-fno-fast-math','-fms-extensions',
    '-I',(Join-Path $R 'src\shared'),'-I',$S) + $units + @('-lopengl32','-lgdi32','-luser32','-lbcrypt','-o',$out)

if(Test-Path $out){ Remove-Item $out -Force }
& $Zig @cc
if($LASTEXITCODE -ne 0 -or -not (Test-Path $out)){ throw "render build failed (zig exit $LASTEXITCODE)" }
'{0}  {1}' -f (Get-FileHash $out -Algorithm SHA256).Hash.ToLower(),$out
