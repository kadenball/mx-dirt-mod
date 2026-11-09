# Builds the MX Dirt core plugin: build\mx_dirt.dll (install as
# <MX Bikes>\plugins\mx_dirt.dlo, with src\core\mx_dirt.ini as plugins\mx_dirt.ini).
# Needs zig (zig cc). Default: `zig` on PATH; pass -Zig <path to zig.exe> otherwise.
param(
  [string]$Zig = '',
  [string]$OutDir = ''
)
$ErrorActionPreference = 'Stop'
try { (Get-Process -Id $PID).PriorityClass = 'BelowNormal' } catch {}

$root = $PSScriptRoot
if (-not $OutDir) { $OutDir = Join-Path $root 'build' }
if (-not $Zig) {
  $cmd = Get-Command zig -ErrorAction SilentlyContinue
  if ($cmd) { $Zig = $cmd.Source }
  else {
    # Fallback: the zig bundled next to this checkout (..\..\work\build-libs\ziglang).
    $bundled = Join-Path $root '..\..\work\build-libs\ziglang\zig.exe'
    if (Test-Path $bundled) { $Zig = (Resolve-Path $bundled).Path }
  }
}
if (-not $Zig -or -not (Test-Path $Zig)) { throw 'zig not found: put zig on PATH or pass -Zig <path>' }

New-Item -ItemType Directory -Force $OutDir | Out-Null
$env:ZIG_GLOBAL_CACHE_DIR = Join-Path $OutDir 'zig-global-cache'
$env:ZIG_LOCAL_CACHE_DIR = Join-Path $OutDir 'zig-local-cache'

$core = Join-Path $root 'src\core'
$shared = Join-Path $root 'src\shared'
$units = @(
  'mx_dirt.c',
  'mxdirt_session.c',
  'mxdirt_contact.c',
  'mxdirt_wheel_sample.c',
  'mxdirt_frame_snapshot.c',
  'mxdirt_height_field.c',
  'mxdirt_height_publication.c',
  'mxdirt_publication_hooks.c',
  'mxdirt_transport.c',
  'mxdirt_native_height.c',
  'mxdirt_patches.c'
) | ForEach-Object { Join-Path $core $_ }

$dll = Join-Path $OutDir 'mx_dirt.dll'
$flags = @('cc', '-shared', '-target', 'x86_64-windows-gnu', '-std=c11', '-O2', '-msse2',
  '-Wall', '-Wextra', '-Werror', '-ffp-contract=off', '-fno-fast-math', '-fms-extensions',
  '-I', $core, '-I', $shared)
$libs = @('-luser32', '-ladvapi32')
& $Zig @flags @units @libs -o $dll
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }
Copy-Item (Join-Path $core 'mx_dirt.ini') (Join-Path $OutDir 'mx_dirt.ini') -Force
$hash = (Get-FileHash $dll -Algorithm SHA256).Hash.ToLower()
"built $dll"
"sha256 $hash"
