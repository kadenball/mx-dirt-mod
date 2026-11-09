# MX Dirt Mod installer. Copies the two plugins, their settings and the launcher into
# MX Bikes\plugins, after backing up anything it replaces. Nothing is ever deleted:
# replaced or retired files are moved to Documents\PiBoSo\MX Bikes\mx_dirt_mod_backups.
param([string]$GameDir='',[switch]$NoShortcut,[switch]$Quiet)
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Windows.Forms
$Here=Split-Path -Parent $PSScriptRoot   # release root (install\ lives inside it)
function Say($m){ if($Quiet){ Write-Host $m } else { [Windows.Forms.MessageBox]::Show($m,'MX Dirt Mod') | Out-Null } }

function Find-Game {
    $roots=@(); try{ $s=(Get-ItemProperty 'HKCU:\Software\Valve\Steam' -ErrorAction Stop).SteamPath; if($s){ $s=$s -replace '/','\'; $roots+=$s
        $vdf="$s\steamapps\libraryfolders.vdf"; if(Test-Path $vdf){ $roots+=(Select-String -Path $vdf -Pattern '"path"\s+"([^"]+)"' | % { $_.Matches[0].Groups[1].Value -replace '\\\\','\' }) } } }catch{}
    $roots+="${env:ProgramFiles(x86)}\Steam"
    foreach($r in $roots){ $g="$r\steamapps\common\MX Bikes"; if(Test-Path "$g\mxbikes.exe"){return $g} }
    $dlg=New-Object Windows.Forms.FolderBrowserDialog;$dlg.Description='Select your MX Bikes folder (the one with mxbikes.exe)'
    if($dlg.ShowDialog() -eq 'OK' -and (Test-Path "$($dlg.SelectedPath)\mxbikes.exe")){return $dlg.SelectedPath}
    throw 'MX Bikes folder not found.' }

try{
    # files from a downloaded zip carry Windows' "downloaded from the internet" mark: clear it on our own files
    Get-ChildItem $Here -Recurse -File -ErrorAction SilentlyContinue | Unblock-File -ErrorAction SilentlyContinue
    if(-not $GameDir){ $GameDir=Find-Game }
    if(Get-Process -Name mxbikes -ErrorAction SilentlyContinue){ throw 'Close MX Bikes first, then run the installer again.' }
    $P="$GameDir\plugins"; New-Item -ItemType Directory -Force $P | Out-Null
    $hash=(Get-FileHash "$GameDir\mxbikes.exe" -Algorithm SHA256).Hash.ToLower()
    $supported=(Get-Content "$Here\SUPPORTED_EXE.txt" -ErrorAction SilentlyContinue | ? { $_ -match '^[0-9a-f]{64}' } | % { ($_ -split '\s')[0] })
    $warn=if($supported -and $supported -notcontains $hash){"`n`nWARNING: your mxbikes.exe is not the version this release supports. The mod will stay inactive (the game runs normally) until an updated release is out."}else{''}

    $B="$([Environment]::GetFolderPath('MyDocuments'))\PiBoSo\MX Bikes\mx_dirt_mod_backups\install-$(Get-Date -Format 'yyyyMMdd-HHmmss')"
    New-Item -ItemType Directory -Force $B | Out-Null
    function Backup($f){ if(Test-Path $f){ Copy-Item $f $B -Recurse -Force } }
    function Retire($f){ if(Test-Path $f){ Move-Item $f $B -Force } }

    # pre-release development builds of this mod and the separate Living Dirt plugin patch the same game code: retire them
    foreach($n in 'mxb_reflex_active.dlo','mxb_reflex_renderer.dlo','living_dirt.dlo','mxb_reflex_runtime_probe.dlo','mxb_publication_probe.dlo','mxb_scoped_render_probe.dlo'){ Retire "$P\$n" }

    foreach($n in 'mx_dirt.dlo','mx_dirt_render.dlo','mx_dirt.ini','mx_dirt_render.ini'){ Backup "$P\$n" }
    Copy-Item "$Here\plugins\mx_dirt.dlo","$Here\plugins\mx_dirt_render.dlo" $P -Force
    # keep the player's settings on an update
    foreach($n in 'mx_dirt.ini','mx_dirt_render.ini'){ if(-not (Test-Path "$P\$n")){ Copy-Item "$Here\plugins\$n" $P } }

    $L="$P\mx_dirt_launcher"; Backup $L; New-Item -ItemType Directory -Force $L | Out-Null
    Copy-Item "$Here\launcher\*" $L -Recurse -Force
    if(-not $NoShortcut){
        $ws=New-Object -ComObject WScript.Shell; $lnk=$ws.CreateShortcut("$([Environment]::GetFolderPath('Desktop'))\MX Dirt Mod.lnk")
        $lnk.TargetPath="$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe"
        $lnk.Arguments="-NoProfile -ExecutionPolicy Bypass -STA -WindowStyle Hidden -File `"$L\MXDirtLauncher.ps1`""
        $lnk.IconLocation="$GameDir\mxbikes.exe,0"; $lnk.WorkingDirectory=$L; $lnk.Save() }
    Say "MX Dirt Mod installed in:`n$P`n`nBackup of replaced files:`n$B`n`nOpen 'MX Dirt Mod' on your desktop to change settings.$warn"
}catch{ Say "Install failed:`n$($_.Exception.Message)"; exit 1 }
