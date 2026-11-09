# MX Dirt Mod launcher: turn the mod on/off, pick how worn tracks start, tune the dirt,
# reset a track's saved wear, start MX Bikes. Settings are read by the plugins when the
# game starts, so changes apply on the next launch.
param([string]$GameDir='',
      [string]$ProfileDir="$([Environment]::GetFolderPath('MyDocuments'))\PiBoSo\MX Bikes\profiles",
      [string]$BackupDir="$([Environment]::GetFolderPath('MyDocuments'))\PiBoSo\MX Bikes\mx_dirt_mod_backups",
      [switch]$SelfTest,[string]$Snapshot,[switch]$ApplyRecommended)
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Windows.Forms,System.Drawing
[System.Windows.Forms.Application]::EnableVisualStyles()

# ---- find MX Bikes: next to this script (plugins\mx_dirt_mod), then Steam libraries, then ask ----
function Find-Game {
    $d=$PSScriptRoot; for($i=0;$i -lt 4 -and $d;$i++){ if(Test-Path "$d\mxbikes.exe"){return $d}; $d=Split-Path $d }
    $roots=@(); try{ $s=(Get-ItemProperty 'HKCU:\Software\Valve\Steam' -ErrorAction Stop).SteamPath; if($s){ $s=$s -replace '/','\'; $roots+=$s
        $vdf="$s\steamapps\libraryfolders.vdf"; if(Test-Path $vdf){ $roots+=(Select-String -Path $vdf -Pattern '"path"\s+"([^"]+)"' | % { $_.Matches[0].Groups[1].Value -replace '\\\\','\' }) } } }catch{}
    $roots+="${env:ProgramFiles(x86)}\Steam"
    foreach($r in $roots){ $g="$r\steamapps\common\MX Bikes"; if(Test-Path "$g\mxbikes.exe"){return $g} }
    $dlg=New-Object Windows.Forms.FolderBrowserDialog;$dlg.Description='Select your MX Bikes folder (the one with mxbikes.exe)'
    if($dlg.ShowDialog() -eq 'OK' -and (Test-Path "$($dlg.SelectedPath)\mxbikes.exe")){return $dlg.SelectedPath}
    throw 'MX Bikes folder not found.' }
try{ if(-not $GameDir){ $GameDir=Find-Game }
     if(-not (Test-Path "$GameDir\plugins\mx_dirt.ini") -and -not (Test-Path "$GameDir\plugins\mxb_reflex_active.ini")){ throw "The dirt mod is not installed in:`n$GameDir`n`nRun Install.cmd from the release first." } }
catch{ [Windows.Forms.MessageBox]::Show("$($_.Exception.Message)",'MX Dirt Mod','OK','Error') | Out-Null; return }

$Plugins="$GameDir\plugins"
# release layout (mx_dirt + mx_dirt_render) or a pre-release development layout
if(Test-Path "$Plugins\mx_dirt.ini"){ $Ini=@{active="$Plugins\mx_dirt.ini";renderer="$Plugins\mx_dirt_render.ini"} }
else{ $Ini=@{active="$Plugins\mxb_reflex_active.ini";renderer="$Plugins\mxb_reflex_renderer.ini";ld="$Plugins\living_dirt.ini"} }
$Enc=[Text.Encoding]::Default

# ---- ini helpers (keep comments and order; replace or add one key) ----
function Read-Lines($p){ ,([Collections.Generic.List[string]]([IO.File]::ReadAllLines($p,$Enc))) }
function Get-Val($lines,$sec,$key,$def){
    $in=$false
    foreach($l in $lines){ $t=$l.Trim()
        if($t -match '^\[(.+)\]$'){ $in=($Matches[1] -eq $sec); continue }
        if($in -and $t -match "^$([regex]::Escape($key))\s*=(.*)$"){ return $Matches[1].Trim() } }
    $def }
function Set-Val($lines,$sec,$key,$val){
    $in=$false;$last=-1;$secAt=-1
    for($i=0;$i -lt $lines.Count;$i++){ $t=$lines[$i].Trim()
        if($t -match '^\[(.+)\]$'){ if($in){break}; $in=($Matches[1] -eq $sec); if($in){$secAt=$i;$last=$i}; continue }
        if($in){ if($t -match "^$([regex]::Escape($key))\s*="){ $lines[$i]="$key=$val"; return }
                 if($t -ne ''){ $last=$i } } }
    if($secAt -lt 0){ $lines.Add(''); $lines.Add("[$sec]"); $lines.Add("$key=$val") } else { $lines.Insert($last+1,"$key=$val") } }

function Game-Running { [bool](Get-Process -Name mxbikes -ErrorAction SilentlyContinue) }

# ---- settings: [file, section, key, label, min, max, recommended, tip] ----
$WearLevels=[ordered]@{ 'Off (fresh track)'=0; 'Practice'=25; 'After moto 1 (Pro look)'=50; 'End of day'=80; 'Destroyed'=130 }
$Num=@(
  @('active','soil','prewear_spread_cm','Wear spread across track (cm)',20,500,140,'How far the virtual riders spread from your lines. Lower = fewer, deeper main lines.'),
  @('active','soil','dirt_sim_depth_percent','Rut depth (%)',10,300,60,'How deep tyres cut. Also follows the game''s erode slider.'),
  @('active','soil','dirt_sim_firmness_percent','Track firmness (%)',0,100,55,'0 = freshly ripped soft dirt, 100 = hardpack.'),
  @('active','soil','dirt_sim_moisture_percent','Moisture (%)',0,100,35,'Wetter dirt holds steeper rut walls and packs better.'),
  @('active','soil','rolling_bumps_percent','Rolling bumps (%)',0,100,60,'Bumps from riding without braking/gas. Braking and acceleration bumps are always on.'),
  @('active','soil','live_gate_lines','Lines riding with you',1,8,1,'1 = only your own line wears. More = virtual riders share your wear on nearby lines.'),
  @('active','soil','soft_layer_mm','Soft dirt layer (mm)',0,100,85,'Loose top soil your tyres press marks into, with lips, roost crumbs and knob prints. Look only, the ruts you ride are unchanged. 0 = off.'),
  @('renderer','renderer','dirt_strength','Dirt look strength (%)',0,200,135,'How strongly worked dirt is shaded.'),
  @('renderer','renderer','dirt_fade_end','Dirt look distance (m)',10,60,60,'Distance where the dirt shading fades out (max 60).')
)
$Chk=@(
  @('renderer','renderer','dirt_material','Dirt shading (ruts, damp dirt, prints)',1),
  @('renderer','renderer','tire_marks','Tyre marks',1),
  @('active','soil','record_ghosts','Record my laps (needed for track wear)',1),
  @('active','soil','prewear_fresh_only','Keep saved wear (only pre-wear fresh tracks)',1)
)

# ---- form ----
$f=New-Object Windows.Forms.Form
$f.Text='MX Dirt Mod';$f.StartPosition='CenterScreen';$f.FormBorderStyle='FixedDialog';$f.MaximizeBox=$false
$f.Font=New-Object Drawing.Font('Segoe UI',9.5);$f.AutoSize=$true;$f.AutoSizeMode='GrowAndShrink';$f.Padding=New-Object Windows.Forms.Padding(12)
$tip=New-Object Windows.Forms.ToolTip;$tip.AutoPopDelay=15000
$tbl=New-Object Windows.Forms.TableLayoutPanel;$tbl.ColumnCount=2;$tbl.AutoSize=$true;$tbl.Dock='Fill'
$f.Controls.Add($tbl)
function Row($label,$ctl,$t){ $lb=New-Object Windows.Forms.Label;$lb.Text=$label;$lb.AutoSize=$true;$lb.Anchor='Left';$lb.Margin=New-Object Windows.Forms.Padding(0,6,12,6)
    $ctl.Anchor='Left';$tbl.Controls.Add($lb);$tbl.Controls.Add($ctl);if($t){$tip.SetToolTip($lb,$t);$tip.SetToolTip($ctl,$t)} }
function Header($text){ $h=New-Object Windows.Forms.Label;$h.Text=$text;$h.AutoSize=$true;$h.Font=New-Object Drawing.Font('Segoe UI',10,[Drawing.FontStyle]::Bold);$h.Margin=New-Object Windows.Forms.Padding(0,10,0,2)
    $tbl.Controls.Add($h);$tbl.SetColumnSpan($h,2) }

$status=New-Object Windows.Forms.Label;$status.AutoSize=$true;$status.MaximumSize=New-Object Drawing.Size(460,0)
$tbl.Controls.Add($status);$tbl.SetColumnSpan($status,2)

$enabled=New-Object Windows.Forms.CheckBox;$enabled.Text='Dirt mod enabled';$enabled.AutoSize=$true;$enabled.Font=New-Object Drawing.Font('Segoe UI',10,[Drawing.FontStyle]::Bold)
$tbl.Controls.Add($enabled);$tbl.SetColumnSpan($enabled,2)
$tip.SetToolTip($enabled,'Off = MX Bikes runs completely vanilla (the dirt plugins stay unloaded).')

Header 'Track wear when a track loads'
$wear=New-Object Windows.Forms.ComboBox;$wear.DropDownStyle='DropDownList';$wear.Width=220
foreach($k in $WearLevels.Keys){[void]$wear.Items.Add($k)}
Row 'Starting wear' $wear 'Virtual riders ride your recorded laps before you start. "After moto 1" matches the Maryland Pro track. Needs at least one recorded lap on the track. A track with saved wear keeps it - reset the track to wear it again at a new level.'
$ctl=@{}
foreach($n in $Num){ if($n[1] -eq 'renderer' -and -not $hdrR){ Header 'Look'; $hdrR=$true } elseif($n[2] -eq 'dirt_sim_depth_percent'){ Header 'Dirt' }
    $u=New-Object Windows.Forms.NumericUpDown;$u.Minimum=$n[4];$u.Maximum=$n[5];$u.Width=80;$ctl[$n[2]]=$u;Row $n[3] $u $n[7] }
foreach($c in $Chk){ $b=New-Object Windows.Forms.CheckBox;$b.Text=$c[3];$b.AutoSize=$true;$ctl[$c[2]]=$b;$tbl.Controls.Add($b);$tbl.SetColumnSpan($b,2) }

Header 'Saved track wear'
$track=New-Object Windows.Forms.ComboBox;$track.DropDownStyle='DropDownList';$track.Width=260
Row 'Track' $track 'Tracks with saved wear (testing.dfm). Reset moves the file to a backup folder - nothing is deleted.'
$resetTrack=New-Object Windows.Forms.Button;$resetTrack.Text='Reset this track''s wear';$resetTrack.AutoSize=$true
$tbl.Controls.Add($resetTrack);$tbl.SetColumnSpan($resetTrack,2)

$btns=New-Object Windows.Forms.FlowLayoutPanel;$btns.AutoSize=$true;$btns.Margin=New-Object Windows.Forms.Padding(0,14,0,0)
$save=New-Object Windows.Forms.Button;$save.Text='Save';$save.AutoSize=$true
$rec=New-Object Windows.Forms.Button;$rec.Text='Recommended';$rec.AutoSize=$true
$launch=New-Object Windows.Forms.Button;$launch.Text='Save && start MX Bikes';$launch.AutoSize=$true
$btns.Controls.AddRange(@($save,$rec,$launch));$tbl.Controls.Add($btns);$tbl.SetColumnSpan($btns,2)
$tip.SetToolTip($rec,'Fill in the tested settings (not saved until you press Save).')

function Load-Tracks {
    $track.Items.Clear()
    Get-ChildItem $ProfileDir -Recurse -Filter testing.dfm -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending |
        ForEach-Object { [void]$track.Items.Add($_.Directory.Name) ; $script:TrackPath[$_.Directory.Name]=$_.FullName }
    if($track.Items.Count){$track.SelectedIndex=0}; $resetTrack.Enabled=$track.Items.Count -gt 0 }
$script:TrackPath=@{}

function Load-All {
    $script:L=@{}; foreach($k in $Ini.Keys){ $script:L[$k]=Read-Lines $Ini[$k] }
    $enabled.Checked=(Get-Val $L.active 'active' 'enabled' '1') -eq '1' -and (Get-Val $L.renderer 'renderer' 'enabled' '1') -eq '1' -and (-not $L.ld -or (Get-Val $L.ld 'mod' 'enabled' '0') -eq '1')
    $r=[int](Get-Val $L.active 'soil' 'prewear_riders' '50'); $script:LoadedRiders=$r
    $name=($WearLevels.Keys | Where-Object { $WearLevels[$_] -eq $r } | Select-Object -First 1)
    if(-not $name){ $name="Custom ($r riders)"; if(-not $wear.Items.Contains($name)){[void]$wear.Items.Add($name)} }
    $wear.SelectedItem=$name
    foreach($n in $Num){ $v=[int](Get-Val $L[$n[0]] $n[1] $n[2] "$($n[6])"); $ctl[$n[2]].Value=[Math]::Max($n[4],[Math]::Min($n[5],$v)) }
    foreach($c in $Chk){ $ctl[$c[2]].Checked=(Get-Val $L[$c[0]] $c[1] $c[2] "$($c[4])") -eq '1' }
    Load-Tracks; Update-Status }
function Update-Status {
    $status.ForeColor=[Drawing.Color]::DimGray
    $status.Text=if(Game-Running){'MX Bikes is running: saved changes apply the next time you start it.'}else{'Changes apply the next time MX Bikes starts.'}
    $resetTrack.Enabled=$track.Items.Count -gt 0 -and -not (Game-Running) }

function Save-All {
    $stamp=Get-Date -Format 'yyyyMMdd-HHmmss'
    if(-not $script:BackedUp){ $d="$BackupDir\settings-$stamp";New-Item -ItemType Directory -Force $d | Out-Null
        foreach($k in $Ini.Keys){ Copy-Item $Ini[$k] $d }; $script:BackedUp=$true }
    $on=if($enabled.Checked){'1'}else{'0'}
    Set-Val $L.active 'active' 'enabled' $on; Set-Val $L.renderer 'renderer' 'enabled' $on; if($L.ld){ Set-Val $L.ld 'mod' 'enabled' $on }
    $r=if($WearLevels.Contains("$($wear.SelectedItem)")){$WearLevels["$($wear.SelectedItem)"]}else{[int]("$($wear.SelectedItem)" -replace '\D','')}
    Set-Val $L.active 'soil' 'prewear_riders' "$r"
    foreach($n in $Num){ Set-Val $L[$n[0]] $n[1] $n[2] "$([int]$ctl[$n[2]].Value)" }
    # the renderer drops its dirt pass unless start < end <= 60
    $end=[int]$ctl['dirt_fade_end'].Value; Set-Val $L.renderer 'renderer' 'dirt_fade_start' "$([Math]::Max(5,[int]($end*.75)))"
    foreach($c in $Chk){ Set-Val $L[$c[0]] $c[1] $c[2] $(if($ctl[$c[2]].Checked){'1'}else{'0'}) }
    foreach($k in $Ini.Keys){ [IO.File]::WriteAllLines($Ini[$k],[string[]]$L[$k],$Enc) }
    $status.ForeColor=[Drawing.Color]::DarkGreen
    $status.Text="Saved $(Get-Date -Format 'HH:mm:ss'). " + $(if(Game-Running){'Restart MX Bikes to apply.'}else{'Applies when MX Bikes starts.'})
    # a new wear level only reaches tracks without saved wear
    if($r -ne $script:LoadedRiders -and $r -gt 0 -and $ctl['prewear_fresh_only'].Checked -and $track.Items.Count -and -not $SelfTest -and -not $ApplyRecommended -and -not (Game-Running)){
        $script:LoadedRiders=$r
        if([Windows.Forms.MessageBox]::Show("Tracks you already rode keep their saved wear.`n`nReset '$($track.SelectedItem)' so it starts at the new level?",'New wear level','YesNo','Question') -eq 'Yes'){ Reset-Track $false } } }

function Apply-Recommended { $wear.SelectedItem='After moto 1 (Pro look)'; foreach($n in $Num){ $ctl[$n[2]].Value=$n[6] }; foreach($c in $Chk){ $ctl[$c[2]].Checked=$c[4] -eq 1 }
    $status.ForeColor=[Drawing.Color]::DimGray; $status.Text='Recommended settings filled in - press Save.' }
$rec.Add_Click({ Apply-Recommended })
$save.Add_Click({ try{ Save-All }catch{ [Windows.Forms.MessageBox]::Show("Could not save:`n$($_.Exception.Message)",'MX Dirt Mod','OK','Error') | Out-Null } })
$launch.Add_Click({ try{ Save-All; if(Game-Running){ $status.Text='MX Bikes is already running - restart it to apply.'; return }
        Start-Process 'steam://rungameid/655500'; $f.Close() }catch{ [Windows.Forms.MessageBox]::Show("Could not start:`n$($_.Exception.Message)",'MX Dirt Mod','OK','Error') | Out-Null } })
function Reset-Track($ask){
    if(Game-Running){ Update-Status; return }
    $n="$($track.SelectedItem)"; $p=$TrackPath[$n]; if(-not $p){return}
    if($ask -and [Windows.Forms.MessageBox]::Show("Start '$n' fresh next time?`n`nIts saved wear is moved to:`n$BackupDir\track-wear",'Reset track wear','OKCancel','Question') -ne 'OK'){return}
    $d="$BackupDir\track-wear";New-Item -ItemType Directory -Force $d | Out-Null
    Move-Item $p "$d\$n-$(Get-Date -Format 'yyyyMMdd-HHmmss').dfm"
    Load-Tracks; $status.ForeColor=[Drawing.Color]::DarkGreen; $status.Text="'$n' will start fresh (backup kept)." }
$resetTrack.Add_Click({ Reset-Track $true })
$timer=New-Object Windows.Forms.Timer;$timer.Interval=3000;$timer.Add_Tick({ if($status.ForeColor -ne [Drawing.Color]::DarkGreen){Update-Status} });$timer.Start()

Load-All
if($SelfTest){
    # exercise load -> save on whatever $GameDir points at (use a copy), then report what was written
    $enabled.Checked=$false; $wear.SelectedItem='Practice'; $ctl['prewear_spread_cm'].Value=140; Save-All
    $L2=Read-Lines $Ini.active; "active.enabled=$(Get-Val $L2 'active' 'enabled' '?') riders=$(Get-Val $L2 'soil' 'prewear_riders' '?') spread=$(Get-Val $L2 'soil' 'prewear_spread_cm' '?')"
    $R2=Read-Lines $Ini.renderer; "renderer.enabled=$(Get-Val $R2 'renderer' 'enabled' '?') fade=$(Get-Val $R2 'renderer' 'dirt_fade_start' '?')..$(Get-Val $R2 'renderer' 'dirt_fade_end' '?')"
    "layout=$(if($Ini.ld){'dev'}else{'release'}) tracks=$($track.Items.Count)"
    $f.Dispose(); return }
if($ApplyRecommended){ Apply-Recommended; Save-All; "saved: $($status.Text)"; $f.Dispose(); return }
if($Snapshot){ $f.Show(); [Windows.Forms.Application]::DoEvents(); $bmp=New-Object Drawing.Bitmap($f.Width,$f.Height); $f.DrawToBitmap($bmp,(New-Object Drawing.Rectangle(0,0,$f.Width,$f.Height))); $bmp.Save($Snapshot); $f.Close(); return }
[void]$f.ShowDialog()
