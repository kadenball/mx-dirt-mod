# MX Dirt Mod

Realistic track wear for **MX Bikes**. Ruts, braking bumps, acceleration chop, rolling bumps, berms and a soft layer of loose dirt all form where and how riders actually wear a track: while you ride, and before you start.

The ruts and bumps are written into MX Bikes' own terrain, so **the bike really rides them**. On top of that, a soft layer of loose dirt takes every tyre mark, the way snow does.

---

## Contents

1. [Quick start](#quick-start)
2. [Requirements](#requirements)
3. [Install](#install)
4. [Your first ride](#your-first-ride)
5. [What every part of the mod does](#what-every-part-of-the-mod-does)
6. [The launcher](#the-launcher)
7. [Settings reference](#settings-reference)
8. [Uninstall](#uninstall)
9. [Updating](#updating)
10. [Troubleshooting](#troubleshooting)
11. [Files the mod creates](#files-the-mod-creates)
12. [How it works (technical)](#how-it-works-technical)
13. [Build from source](#build-from-source)
14. [License](#license)

---

## Quick start

1. Close MX Bikes.
2. Download `MX-Dirt-Mod-<version>.zip` from **Releases** (right side of the GitHub page) and extract it anywhere, for example to your Desktop.
3. Double-click **`Install.cmd`**.
4. Start MX Bikes, open a track in **Testing** and ride a few laps.
5. Reset the track's wear in the **MX Dirt Mod** launcher (desktop shortcut) and load the track again. It now starts worn, as if a moto had already been ridden on it.

To remove the mod, double-click **`Uninstall.cmd`** in the same folder.

---

## Requirements

- Windows 10 or 11, 64-bit.
- **MX Bikes from Steam, build 23566986.** The exact `mxbikes.exe` hash is in `SUPPORTED_EXE.txt`.
  - The mod hooks fixed places in that exact executable.
  - With any other game version it switches itself off and the game runs normally, until a release for that version is out. Nothing breaks.
- A graphics card with OpenGL 4 (any GPU from the last ten years).
- **Testing sessions, offline.** The mod does nothing in online races.

You do **not** need to install anything else: no downloads, no Python, no admin rights.

---

## Install

1. **Close MX Bikes.** The installer refuses to run while the game is open.
2. **Extract the zip.** Right-click the zip, choose *Extract All…*, and pick a folder. Don't run it from inside the zip.
3. **Double-click `Install.cmd`.**
   - It finds your MX Bikes folder through Steam. If it can't, it asks you to pick the folder that contains `mxbikes.exe`.
   - A message box tells you where it installed and where it put its backup.

That's it. The installer:

- copies two plugins into `MX Bikes\plugins`:
  - `mx_dirt.dlo`, the dirt simulation
  - `mx_dirt_render.dlo`, the dirt look
- copies their settings files, `mx_dirt.ini` and `mx_dirt_render.ini`. On an update it **keeps your existing settings**.
- installs the launcher to `MX Bikes\plugins\mx_dirt_launcher` and puts an **"MX Dirt Mod" shortcut on your desktop**.
- **never deletes anything.** Every file it replaces is moved to `Documents\PiBoSo\MX Bikes\mx_dirt_mod_backups\install-<date>`.

If Windows shows a blue "Windows protected your PC" box, click *More info* → *Run anyway*. The installer is a plain PowerShell script (`install\Install.ps1`) that you can read first.

**Installing by hand:** copy everything inside the zip's `plugins` folder into `MX Bikes\plugins`. That's all the game needs. The launcher is optional.

---

## Your first ride

Pre-wear (a worn track when it loads) rides **your own recorded laps**, so it needs at least one lap on that track first:

1. Start MX Bikes and open a track in **Testing**.
2. Ride a few clean laps. Every valid lap is recorded automatically to `plugins\mx_dirt_laps\<track>`. Ruts, bumps and soft-layer marks form under your wheels as you ride.
3. Leave the track and close MX Bikes.
4. Open the **MX Dirt Mod** launcher, pick the track under *Saved track wear* and press **Reset this track's wear**.
   - This makes the track start fresh next time.
   - Its old saved wear is moved to the backup folder.
5. Start MX Bikes and load the track again. Virtual riders ride your recorded laps before you start, so loading takes about 10 seconds longer. The track starts looking like it's after moto 1.

From then on the track keeps its wear between sessions. MX Bikes saves it when you leave, and the mod saves its soft-layer marks with it.

---

## What every part of the mod does

### 1. Live dirt simulation (`mx_dirt.dlo`)
Every tyre contact, every physics frame, works the soil under it:
- **Ruts:**
  - Tyres sink by their load into soil that gets firmer the more it is ridden.
  - Fresh dirt cuts deep. A ridden-in line packs hard and cuts little.
  - Tyres drop into existing grooves, so a few deep parallel ruts form instead of one wide trench.
- **Loose soil and berms:**
  - Cut soil is pushed out as loose dirt.
  - Loose dirt slides down to its natural angle (about 34°). Damp soil holds steeper walls.
  - Corners build berms where riders push the soil out.
- **Braking bumps and acceleration chop:**
  - The wheel's own bounce (fork, shock, tyre) decides where bumps form, the same way it does on a real track.
  - Braking rows get tallest. Hard acceleration builds chop.
  - Any hop makes small rollers.
- **Landings** pack the ground rather than digging craters.
- **Roost:** spinning tyres throw soil back as scattered clods.
- **Moisture:** the track starts watered and dries out over a ride. Damp soil is darker, packs better and holds sharper ruts.
- **Surfaces** use MX Bikes' own material under the tyre:
  - sand ruts deep and wide
  - hard-pack resists and grows chop
  - soft soil gives more
  - gravel barely moves

The wear goes into MX Bikes' terrain deformation, so the game's physics and your bike ride it.

### 2. Pre-wear
- When a **fresh** track loads, a gate of virtual riders (50 by default) rides your four fastest recorded laps.
- Each rider has its own line, spread and aggression, its own bike setup, and its own braking and gas points.
- "After moto 1" was tuned against a real pro-worn track (bump heights, rut depth, worn band width).
- A track that already has saved wear is left as it is, so wear doesn't pile up on every load. Use the launcher's reset to start a track over.

### 3. Lap recording
- Every valid lap is saved as a `.dghost` file in `plugins\mx_dirt_laps\<track>\`.
- These files hold only your tyre contacts (position, speed, slip, load), not a replay.
- Pre-wear uses the fastest four. You can delete old ones any time.

### 4. Soft dirt layer
- A few centimetres of loose top soil (85 mm by default) lie on the dirt, like fresh snow but dirt.
- Tyres press trenches into it:
  - with knob prints in the trench floor
  - with lips of dirt pushed up beside the tread
  - with crumbs roosted back when you spin
  - with a mound shoved ahead when you lock the brakes
  - pushed to one side when you slide
- It follows the dirt simulation:
  - It lies deeper on loose soil and on a softer track.
  - It is thinner, and ironed flat, on packed lines.
  - Damp dirt holds crisp prints and tall lips; dry dirt crumbles.
  - Sand lies deep; hard-pack and gravel carry only a thin skin.
- Up close it's drawn with real depth (parallax) and its own shadows.
- It's **a look only**: the bike rides the dirt simulation's ruts under it.
- Its marks are saved with the track in `plugins\mx_dirt_soft\<track>\`.

### 5. Dirt look (`mx_dirt_render.dlo`)
- Shades the ground MX Bikes just drew, using the simulation's state, so every track keeps its own dirt colour.
- Sharper rut walls and lighting, with shadows cast by the ruts.
- Damp rut bottoms, dry crests, packed shiny lines and loose crumbly dirt.
- Tyre prints, roost clods, and the soft dirt layer.
- It fades out with distance (60 m by default) and leaves grass alone.

### 6. Engine fixes
While a track is loaded, the mod applies three small, guarded fixes to MX Bikes' terrain code in memory, and removes them when you leave the track:

| Fix | Why the mod needs it |
|---|---|
| **Signed deformation merge** | The game's deformation can normally only dig. This lets berms and pushed-up soil exist. |
| **Non-negative material lookup** | Raised ground still finds its surface material. |
| **Raised dirt surface** (render plugin) | Raised ground shows the dirt surface, not the bottom texture layer. |

You can turn off the raised dirt surface fix with `raised_dirt_surface=0` in `mx_dirt_render.ini`.

### 7. Launcher
A small settings window. See [The launcher](#the-launcher).

---

## The launcher

Open **MX Dirt Mod** from your desktop. If the shortcut is missing, run `MX Bikes\plugins\mx_dirt_launcher\MX Dirt Mod.cmd`.

- **Dirt mod enabled:** turns the whole mod on or off. Off means MX Bikes runs exactly as without it.
- **Starting wear:** how worn a fresh track starts.
  - Off
  - Practice (25 riders)
  - After moto 1 (50, the default)
  - End of day (80)
  - Destroyed (130)
- **Wear spread:** how far the virtual riders spread from your lines. Lower gives fewer, deeper lines.
- **Dirt:** rut depth, track firmness, moisture, rolling bumps, lines riding with you, and the soft dirt layer thickness.
- **Look:** dirt look strength and distance, dirt shading on/off, tyre marks on/off.
- **Record my laps:** needed for pre-wear.
- **Keep saved wear:** pre-wear only fresh tracks. Turn it off to wear the track again on every load, so wear stacks up.
- **Saved track wear → Reset this track's wear:** the chosen track starts fresh next time. Its saved wear is moved to the backup folder, not deleted.
- **Recommended:** fills in the default settings; press Save to keep them.
- **Save / Save & start MX Bikes**

**Changes apply the next time MX Bikes starts.**

---

## Settings reference

All settings live in two text files in `MX Bikes\plugins`, with comments. The launcher edits them for you.

### `mx_dirt.ini` (simulation)

| Key | Default | Range | What it does |
|---|---|---|---|
| `[active] enabled` | 1 | 0/1 | 0 turns the simulation plugin off. |
| `dirt_sim_depth_percent` | 60 | 10–300 | How deep tyres cut. Also follows the in-game Deformation slider. |
| `dirt_sim_firmness_percent` | 55 | 0–100 | 0 = freshly ripped soft dirt, 100 = hard-pack. |
| `dirt_sim_moisture_percent` | 35 | 0–100 | 0 dry/dusty, 35 ideal, 100 muddy. |
| `packed_hardness_percent` | 100 | 25–200 | How hard a ridden-in line gets. |
| `breakdown_percent` | 100 | 0–300 | How fast sliding tyres break packed crust loose. |
| `drying_percent` | 100 | 0–400 | How fast the track dries during a ride. |
| `sweep_percent` | 100 | 0–300 | How strongly tyres push loose soil off the line. |
| `rut_capture_percent` | 100 | 0–300 | Tyres dropping into existing grooves. |
| `wall_cohesion_percent` | 100 | 0–300 | Damp rut walls holding steep. |
| `roost_clods_percent` | 100 | 0–300 | Roost landing as clods. |
| `sharp_ruts` | 0 | 0/1 | Sharper ruts; diagonal ruts can show teeth. |
| `braking_bumps_percent` | 100 | 0–200 | Braking bumps and bump stress. |
| `rolling_bumps_percent` | 60 | 0–100 | Bumps without braking or gas. |
| `live_gate_lines` | 1 | 1–8 | Your live wear shared over N nearby lines. |
| `extra_riders` | 0 | 0–15 | Test mode: extra riders copy each of your contacts. |
| `record_ghosts` | 1 | 0/1 | Record your laps. |
| `ghost_riders` | 0 | 0–12 | Recorded laps replayed live as invisible riders. |
| `prewear_riders` | 50 | 0–400 | Virtual riders on a fresh track (0 = off). |
| `prewear_spread_cm` | 140 | 0–500 | How far they spread across the track. |
| `prewear_clip_cm` | 400 | 0–600 | Farthest any of them strays. |
| `prewear_fresh_only` | 1 | 0/1 | Pre-wear only tracks without saved wear. |
| `soft_layer_mm` | 85 | 0–100 | Soft dirt layer thickness (0 = off). |
| `follow_game_deformation` | 1 | 0/1 | Rut depth follows the in-game Deformation slider. |
| `game_deformation_reference_percent` | 25 | 1–200 | Slider value at which rut depth applies unchanged. |
| `sand_tracks` | (empty) | text | Track-name fragments to treat as sand, comma separated. |
| `[debug] dump_terrain` | 0 | 0/1 | Save each track's terrain for offline analysis. |

An out-of-range value switches the simulation off for that game start, and the reason is written to `plugins\mx_dirt.jsonl`.

### `mx_dirt_render.ini` (look)

| Key | Default | What it does |
|---|---|---|
| `enabled` | 1 | 0 turns the look plugin off. |
| `dirt_material` | 1 | The dirt shading pass. |
| `dirt_strength` | 135 | Strength of the shading, 0–200 %. |
| `dirt_loose` | 55 | Fine loose grain on worked ground, 0–100 %. |
| `dirt_fade_start` / `dirt_fade_end` | 45 / 60 | Distance (m) where the look fades out; the end is 60 at most. |
| `tire_marks` | 1 | Tyre prints from the simulation. |
| `soft_layer` | 1 | Draw the soft dirt layer. |
| `raised_dirt_surface` | 1 | Raised-dirt surface fix. |
| `content_log` | `plugins\mx_dirt_render.log` | Diagnostic log; empty = none. |

---

## Uninstall

1. Close MX Bikes.
2. Double-click **`Uninstall.cmd`** from the release folder you installed from. If you deleted that folder, download the release again and use its `Uninstall.cmd`.

The uninstaller **moves** these to `Documents\PiBoSo\MX Bikes\mx_dirt_mod_backups\uninstall-<date>`, so nothing is deleted:
- both plugins and their settings
- logs
- the launcher and the desktop shortcut
- the soft-layer marks

After that, MX Bikes is back to normal.

What stays:

- **Your recorded laps** in `MX Bikes\plugins\mx_dirt_laps`. Keep them if you might reinstall, or delete the folder.
- **Tracks' saved wear.** MX Bikes itself saves each track's terrain when you leave it, and those ruts stay in the game's own save.
  - To make a track fresh again, move or delete its `testing.dfm` from `Documents\PiBoSo\MX Bikes\profiles\<profile>\tracks\<track>\`.
  - The launcher's *Reset this track's wear* does exactly that, so you can use it before uninstalling.
- **The backup folder** `Documents\PiBoSo\MX Bikes\mx_dirt_mod_backups`. Delete it when you no longer need it.

**Uninstalling by hand:** with the game closed, remove these from `MX Bikes\plugins`:
- `mx_dirt.dlo` and `mx_dirt_render.dlo`
- `mx_dirt.ini` and `mx_dirt_render.ini`

**Turning it off without uninstalling:** untick *Dirt mod enabled* in the launcher, or set `enabled=0` in both ini files.

---

## Updating

Close MX Bikes, extract the new release and run its `Install.cmd`. Your settings, laps and saved wear are kept. The old plugins go to the backup folder.

If a new release adds new settings, they use their defaults until you change them in the launcher. Press **Recommended** and then **Save** to take all the new defaults.

---

## Troubleshooting

**Nothing happens: no ruts, no look.**
- Check you are in a **Testing** session, offline.
- Open `MX Bikes\plugins\mx_dirt.jsonl` in Notepad. The first line shows `exe_sha256`. If it doesn't match `SUPPORTED_EXE.txt`, your MX Bikes version isn't supported yet, and the mod stays off on purpose.
- If the file is missing, the plugin didn't load. Check that `mx_dirt.dlo` is in `MX Bikes\plugins`, and that `enabled=1`.

**The track doesn't start worn.**
- Pre-wear needs recorded laps on that track: ride a few laps first.
- It only runs on a fresh track. Use the launcher's *Reset this track's wear*.
- Look for `"kind":"prewear"` in `mx_dirt.jsonl`.

**No dirt look, but ruts work.**
- Check that `mx_dirt_render.dlo` is installed and that `enabled=1` in `mx_dirt_render.ini`.
- Open `plugins\mx_dirt_render.log`. A `dirt_disabled` line says why the look is off.

**Loading a fresh track takes longer.** That's pre-wear riding 50 virtual riders: around 10 seconds, and only on a fresh track. Lower *Starting wear* to speed it up.

**The installer says "Close MX Bikes first".** The game, or a stuck `mxbikes.exe`, is still running. End it in Task Manager.

**I want my old track back.** Every reset or replaced file is in `Documents\PiBoSo\MX Bikes\mx_dirt_mod_backups`. To restore a track, copy its `.dfm` from `track-wear` back to that track's folder and rename it `testing.dfm`.

When you report a problem, attach `plugins\mx_dirt.jsonl` and `plugins\mx_dirt_render.log`.

---

## Files the mod creates

| Where | What |
|---|---|
| `MX Bikes\plugins\mx_dirt.dlo`, `mx_dirt.ini` | Simulation plugin and its settings |
| `MX Bikes\plugins\mx_dirt_render.dlo`, `mx_dirt_render.ini` | Look plugin and its settings |
| `MX Bikes\plugins\mx_dirt_launcher\` | The launcher |
| `MX Bikes\plugins\mx_dirt_laps\<track>\*.dghost` | Your recorded laps |
| `MX Bikes\plugins\mx_dirt_soft\<track>\layer.msl` | Soft dirt layer marks for the track's saved wear |
| `MX Bikes\plugins\mx_dirt.jsonl`, `mx_dirt_render.log` | Logs (overwritten every game start) |
| `Desktop\MX Dirt Mod.lnk` | Launcher shortcut |
| `Documents\PiBoSo\MX Bikes\mx_dirt_mod_backups\` | Everything the installer, uninstaller or a reset replaced |

The mod never changes `mxbikes.exe` or any game file on disk. Its engine fixes live only in memory while a track is loaded.

---

## How it works (technical)

MX Bikes loads every `.dlo` in its `plugins` folder through its plugin API. This mod is two plugins:

**Core: `mx_dirt.dlo`**
- **Startup:**
  - Hashes `mxbikes.exe` and refuses to run on anything but the supported build.
  - Reads `mx_dirt.ini`.
- **Track load:** installs the two terrain fixes (`mxdirt_patches.c`).
- **Session setup:**
  - Hooks the per-physics-step wheel-force call and the game's outer scheduler step.
  - Verifies the game's two terrain copies agree.
  - Builds the dirt simulation over the track's height grid.
  - Runs pre-wear.
- **Every frame:**
  1. Wheel contacts (position, load, slip, material) are read from the bike (`mxdirt_wheel_sample.c`, `mxdirt_contact.c`).
  2. The dirt simulation steps (`mxb_soil_sim.h`), together with work history (`mxdirt_soil_evolution.h`), bump stress (`mxb_soil_bumps.h`) and the soft layer (`mxdirt_soft_layer.h`).
  3. Changed heights are converted to MX Bikes' deformation format (`mxdirt_height_field.c`).
  4. They are published into the game's terrain, sparsely and paced (`mxdirt_transport.c`, `mxdirt_height_publication.c`).
- **Track close:**
  - Saves the soft layer.
  - Unhooks.
  - Removes the terrain fixes.
- **Export:** `MxDirtBorrowFrame` shares the simulation's read-only state with the render plugin (`src/shared/mxdirt_frame.h`).

**Render: `mx_dirt_render.dlo`**
- Hooks the terrain draw.
- Right after MX Bikes draws the ground, it draws one extra pass over a ~70 m window around the camera, using the game's own camera and sun:
  - it reads the native colour that was just drawn
  - it reads the current and original heights, and the simulation state
  - it reads the soft layer: a 51 m window, uploaded tile by tile as it changes
  - it re-lights the ground (`mxdirt_dirt_shader.h`)
- It never writes depth, so the bike and everything else draw over it normally.

**Safety**
- Every hook and patch checks the exact bytes it replaces, and puts them back.
- Any mismatch, fault or unexpected thread switches the mod off for that track, and the game carries on.

---

## Build from source

You need [Zig](https://ziglang.org/) 0.13 or newer on `PATH`; it is used as the C compiler. In PowerShell:

```
.\build.ps1                    # core plugin   -> build\mx_dirt.dll   (install as plugins\mx_dirt.dlo)
.\build_render.ps1             # render plugin -> build\mx_dirt_render.dll
.\package.ps1 -Version 1.1.0   # both + the release zip in build\
```

Source layout:
- `src\core`: the dirt simulation (`mxb_soil_*.h`), the soft dirt layer (`mxdirt_soft_layer.h`), pre-wear, lap recording, engine fixes and publishing heights into the game.
- `src\render`: the dirt look pass and its shader.
- `src\shared`: the interface between the two plugins.
- `install`, `launcher`: the installer, uninstaller and launcher (PowerShell).

---

## License

GPL-3.0, see `LICENSE`. MX Bikes is © PiBoSo. This project is not affiliated with or endorsed by PiBoSo.
