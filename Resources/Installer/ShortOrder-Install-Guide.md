# ShortOrder: Install Guide (macOS)

ShortOrder, "The 8ms Guitar Synthesizer", by Yacht Pockets. It hears your guitar's pitch and plays a monophonic synth voice, with very low, constant latency.

## What you received

Signed and notarized by Yacht Pockets:

<!--PKG-->
- **ShortOrder-@VERSION@-macOS.pkg**: the installer. It installs every format you tick. Use this unless you prefer to copy files yourself.
<!--/PKG-->
- **ShortOrder-AU.zip**: the Audio Unit, for Logic Pro, GarageBand, MainStage and other AU hosts.
- **ShortOrder-VST3.zip**: the VST3, for REAPER, Ableton Live, Cubase, Studio One, Bitwig and other VST3 hosts.
- **ShortOrder-AAX.zip**: the AAX, for Pro Tools. <!--AAXLINE-->
- **ShortOrder-Standalone.zip**: the standalone app, to play without a DAW.
- **SHA256SUMS.txt**: checksums of every file, to check your download (`shasum -a 256 -c SHA256SUMS.txt`).

Each zip also has `LICENSE`, `NOTICE`, `THIRD-PARTY-NOTICES.md` and `OFL.txt` (the font's licence).

## Requirements

- macOS 11 (Big Sur) or later, on an Apple silicon (M-series) or Intel Mac.
- An audio interface with a guitar (instrument / Hi-Z) input.
- For the plug-in: Logic Pro, REAPER, or any AU or VST3 host.
- For Pro Tools: the AAX build. <!--AAXLINE-->

<!--PKG-->
## Install with the installer (recommended)

1. Double-click `ShortOrder-@VERSION@-macOS.pkg`.
2. Click through the introduction and the licence.
3. At **Installation Type**, click **Customize** to choose the formats (all are ticked by default).
4. Enter your Mac's password when asked, and finish.

It installs:

| Format | Where |
|---|---|
| AU | `/Library/Audio/Plug-Ins/Components/ShortOrder.component` |
| VST3 | `/Library/Audio/Plug-Ins/VST3/ShortOrder.vst3` |
| AAX | `/Library/Application Support/Avid/Audio/Plug-Ins/ShortOrder.aaxplugin` | <!--AAXLINE-->
| Standalone | `/Applications/ShortOrder.app` |

Then open your DAW. If ShortOrder doesn't appear, rescan your plug-ins (see Troubleshooting).

<!--/PKG-->
## Install from the zips

1. Unzip the format you want.
2. In Finder, press **Cmd + Shift + G** and paste the folder's path from the table below.
3. Drag the plug-in into that folder. Folders under `/Library` ask for your password.
4. Open your DAW. If ShortOrder doesn't appear, rescan your plug-ins (see Troubleshooting).

| Format | Drag this | Into this folder |
|---|---|---|
| AU | `ShortOrder.component` | `/Library/Audio/Plug-Ins/Components` (every user) or `~/Library/Audio/Plug-Ins/Components` (just you) |
| VST3 | `ShortOrder.vst3` | `/Library/Audio/Plug-Ins/VST3` or `~/Library/Audio/Plug-Ins/VST3` |
| AAX | `ShortOrder.aaxplugin` | `/Library/Application Support/Avid/Audio/Plug-Ins` | <!--AAXLINE-->
| Standalone | `ShortOrder.app` | `/Applications` |

## Set up: Logic Pro (AU)

1. Open **Logic Pro → Settings → Plug-in Manager**. Find ShortOrder (manufacturer "YachtPockets"). If it has a red mark or is missing, select it and click **Reset & Rescan Selection**.
2. Create an **audio track** with your guitar's input. Mono is fine.
3. In an empty **Audio FX** slot, choose **Audio Units → YachtPockets → ShortOrder**.
4. Turn on the track's **input monitoring** (the I button) and play.
5. **Turn on Low Latency Mode** (see Tips): Logic switches it off at every launch.

ShortOrder is an insert on the guitar track itself. There's no sidechain to set up.

<!--AAX-->
## Set up: Pro Tools (AAX)

1. Open Pro Tools. ShortOrder appears under **Pitch Shift** (and under the manufacturer **YachtPockets**).
2. Create a **mono** audio track with your guitar's input. A stereo track works too.
3. Insert **ShortOrder** on that track. On a mono track, choose the **mono** or **mono → stereo** version.
4. Record-enable the track or turn on **TrackInput** monitoring, and play.
<!--/AAX-->

## Set up: REAPER and other VST3 hosts

1. Rescan plug-ins (REAPER: **Settings → Plug-ins → VST → Re-scan**).
2. Create a track with your guitar's input, arm it, and turn on **record monitoring**.
3. Add **ShortOrder** (YachtPockets) to the track's FX chain, and play.

## Set up: the Standalone app

1. Open **ShortOrder** from Applications.
2. The first time, macOS asks whether ShortOrder may use the **microphone**: click **Allow**. That's how macOS names any audio input, your guitar included. (To change it later: **System Settings → Privacy & Security → Microphone**.)
3. Click **Options** (top left) and choose your audio interface, the input your guitar is on, and the output. Set the buffer to **64 samples** or lower.
4. Play.

## Tips for the lowest latency

- **I/O buffer: 64 samples or lower** when you play. The interface's buffer adds delay on top of ShortOrder's (256 adds about 11 ms).
- **Logic: turn on Low Latency Mode.** Add its button to the control bar (Ctrl-click the control bar, **Customize Control Bar and Display**), because Logic switches it off at every launch. Turn off direct or hardware monitoring on your interface, so you hear only the synth.
- **Tune your guitar to A440.** ShortOrder snaps to the nearest note, so a string more than about 40 cents sharp or flat can flip to the neighbouring note.
- **Play a clean, dry signal into it:** a DI guitar, before any amp, drive or delay.
- **The latency knob:** turning it right lowers the latency and raises the lowest playable note. The LED neck shows exactly which frets play at your setting. Notes below that come back with **Octave**.

## Troubleshooting

**The plug-in doesn't appear:** rescan plug-ins in your DAW (Logic: Plug-in Manager; REAPER: Re-scan), or restart the DAW. Check that the file is in the right folder (see the tables above).

**macOS shows a security warning:** ShortOrder is signed and notarized by Apple, so this shouldn't happen. If it does, open Terminal and run the line for your format:

```
sudo xattr -cr /Library/Audio/Plug-Ins/Components/ShortOrder.component
sudo xattr -cr /Library/Audio/Plug-Ins/VST3/ShortOrder.vst3
sudo xattr -cr "/Library/Application Support/Avid/Audio/Plug-Ins/ShortOrder.aaxplugin" <!--AAXLINE-->
sudo xattr -cr /Applications/ShortOrder.app
```

If you copied a plug-in into your own `~/Library` folder instead, use that path (without `sudo`), e.g. `xattr -cr ~/Library/Audio/Plug-Ins/Components/ShortOrder.component`.

**No sound:** check that the track's input is the one your guitar is on, that monitoring is on, and that the latency knob's neck shows the notes you play as playable (green). Notes below the playable zone aren't tracked.

**The Standalone hears nothing:** allow the microphone in **System Settings → Privacy & Security → Microphone**, and pick your interface's input in **Options**.

## Uninstall

Delete whichever of these you installed (Finder: **Cmd + Shift + G**, paste the path, and move the file to the Trash):

- `/Library/Audio/Plug-Ins/Components/ShortOrder.component`
- `/Library/Audio/Plug-Ins/VST3/ShortOrder.vst3`
- `/Library/Application Support/Avid/Audio/Plug-Ins/ShortOrder.aaxplugin` <!--AAXLINE-->
- `/Applications/ShortOrder.app`

(and the same names under `~/Library/Audio/Plug-Ins/` if you put them there).
<!--PKG-->
If you used the installer, you can also make macOS forget its receipts: in Terminal, run `pkgutil --pkgs | grep com.yachtpockets.shortorder | xargs -n 1 sudo pkgutil --forget`.
<!--/PKG-->

## Licence

ShortOrder is free and open source under the MIT License (see `LICENSE`). Third-party notices are in `THIRD-PARTY-NOTICES.md`. ShortOrder and Yacht Pockets are names of Duncan Wold: the licence covers the code, not the names.
