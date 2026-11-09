# MX Dirt Mod uninstaller. Moves the mod's files out of MX Bikes\plugins into
# Documents\PiBoSo\MX Bikes\mx_dirt_mod_backups (nothing is deleted), including the soft-layer
# marks (plugins\mx_dirt_soft). Your recorded laps (plugins\mx_dirt_laps) and the game's own
# saved track wear stay where they are.
param([string]$GameDir='',[switch]$Quiet)
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Windows.Forms
function Say($m){ if($Quiet){ Write-Host $m } else { [Windows.Forms.MessageBox]::Show($m,'MX Dirt Mod') | Out-Null } }
try{
    if(-not $GameDir){
        $d=$PSScriptRoot; for($i=0;$i -lt 4 -and $d;$i++){ if(Test-Path "$d\mxbikes.exe"){$GameDir=$d;break}; $d=Split-Path $d }
        if(-not $GameDir){ $s=((Get-ItemProperty 'HKCU:\Software\Valve\Steam' -ErrorAction SilentlyContinue).SteamPath -replace '/','\')
            foreach($r in @($s,"${env:ProgramFiles(x86)}\Steam")){ if($r -and (Test-Path "$r\steamapps\common\MX Bikes\mxbikes.exe")){$GameDir="$r\steamapps\common\MX Bikes";break} } } }
    if(-not $GameDir){ throw 'MX Bikes folder not found. Run: Uninstall.ps1 -GameDir "<your MX Bikes folder>"' }
    if(Get-Process -Name mxbikes -ErrorAction SilentlyContinue){ throw 'Close MX Bikes first.' }
    $P="$GameDir\plugins"; $B="$([Environment]::GetFolderPath('MyDocuments'))\PiBoSo\MX Bikes\mx_dirt_mod_backups\uninstall-$(Get-Date -Format 'yyyyMMdd-HHmmss')"
    New-Item -ItemType Directory -Force $B | Out-Null
    foreach($n in 'mx_dirt.dlo','mx_dirt_render.dlo','mx_dirt.ini','mx_dirt_render.ini','mx_dirt.jsonl','mx_dirt_render.log','mx_dirt_launcher','mx_dirt_soft','mx_dirt_terrain'){ if(Test-Path "$P\$n"){ Move-Item "$P\$n" $B -Force } }
    $lnk="$([Environment]::GetFolderPath('Desktop'))\MX Dirt Mod.lnk"; if(Test-Path $lnk){ Move-Item $lnk $B -Force }
    Say "MX Dirt Mod removed. The game is back to normal.`n`nRemoved files were moved to:`n$B"
}catch{ Say "Uninstall failed:`n$($_.Exception.Message)"; exit 1 }
