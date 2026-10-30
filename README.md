# XDJ-700 PLUS

Unofficial firmware mod for the Pioneer DJ XDJ-700 that plays the formats the stock player refuses. Based on the official firmware 1.15.

> **Unofficial and experimental. FLASHING FIRMWARE IS AT YOUR OWN RISK.**
> It can leave a player unusable. There is no warranty of any kind, and no one who made or shared it is responsible for any damage or loss.
> So far it has been tested on two players; we do not know whether other hardware revisions exist.
> This is not a Pioneer DJ or AlphaTheta product.

XDJ-700 PLUS is meant to sit alongside the stock functions without disrupting them or the way you use the player. Everything the stock player already plays (MP3, AAC, and WAV and AIFF at 44.1 and 48 kHz) still goes through Pioneer's own code, untouched, and the new formats work the same way: rekordbox waveforms, beat grids and cue points, hot cues, loops, needle search, reverse, jog and tempo, apart from the edge cases under Good to know. More could be added, beat jump and 3-band waveforms among them, but they are set aside for now: only what has proven stable on the player goes in.

## What it adds

- **FLAC**, 16-bit and 24-bit, 44.1 to 96 kHz, stereo or mono, at full depth
- **Hi-res WAV and AIFF**, 88.2 and 96 kHz, stereo or mono
- **Bandcamp WAVs** that the stock player rejects, stereo or mono
- Works from **USB and over LINK**. Over LINK only the player that plays the track needs the mod: the one holding the USB stick can run the official firmware.
- Ignores the "unplayable" mark that another player may have put on a track the mod can play

## The sound

- **Full depth.** 24-bit files stay 24-bit all the way to the player's mixer; nothing is cut to 16 bits.
- **Why 88.2 and 96 kHz are downsampled.** The player's audio output only runs at 44.1 and 48 kHz (we tried to run it at the higher rates: it stays silent). So 88.2 kHz is downsampled to 44.1 kHz and 96 kHz to 48 kHz, an exact halving, so no new rate has to be invented between samples.
- **The filter.** Before downsampling, everything above the new limit has to be removed, or it would fold back into the audible range as noise and false tones. A 47-tap half-band low-pass filter does that: a 20 kHz tone passes changed by 0.007 dB, while a 30 kHz tone, which would otherwise fold down to 18 kHz, comes out 67.8 dB down.

## Good to know

- 88.2 and 96 kHz files are downsampled to 44.1 and 48 kHz respectively.
- Right after loading a FLAC or hi-res file, the screen can be slower than usual for a few moments while the player buffers. The sound is not affected: no delay, no lag, no gaps. It is only visual.
- An edge case with 96 kHz 24-bit FLAC pitched above +50 % with MASTER TEMPO on: spinning the jog wheel very fast in vinyl mode can give short gaps in the sound. The sound recovers as soon as you stop spinning. In extreme cases it can end in the player's emergency loop; loading the same or another track recovers it.
- FLAC files encoded with very large blocks (16,384 frames, which is rare): the same applies to very fast jog spinning, and hitting their hot cues in a frenzy can also give short gaps, so better avoid that. No emergency loop has been seen with these files, and in normal DJ use they have played flawlessly.
- ALAC, 32-bit, 176.4 kHz and 192 kHz files are not supported.
- The update check, other players, rekordbox and the error log still see version 1.15, for compatibility.

## Read this first

- This is not a Pioneer DJ or AlphaTheta product. They have nothing to do with it, do not support it, and may refuse service on a modified player.
- It is free software under the GNU General Public License version 3, provided as is, with no warranty of any kind.
- You use it entirely at your own risk. In no case is anyone who made or shared it responsible for any damage or loss, to your player, your media, your gig or anything else.
- It has been tested at length on the author's XDJ-700s, in sessions of up to 18 hours, and has been very stable. That is not proof that it works the same on yours. Treat it as experimental, and do not rely on it for a show before you have tried it yourself at home.
- A failed or interrupted update can leave a player unusable, as with any firmware update. Never switch the player off during an update.
- The boot and recovery region and the front panel firmware stay as Pioneer made them, so the player's own update can always put the official firmware back.
- Keep the restore file that the patcher writes, in case you want to go back to stock.
- Only for the XDJ-700, and only from your own official XDJ-700 firmware version 1.15 UPD file.

## Install

This repository holds no Pioneer firmware. You bring your own official XDJ-700 firmware version 1.15 UPD file and a FAT32 USB stick with about 18 MB free; the patcher adds the mod to the firmware file on your computer.

1. Download `xdj700_plus_patcher.html` from the [latest release](https://github.com/lowtrustsociety/XDJ-700-PLUS/releases/latest) and open it in a current browser (Chrome, Edge, Firefox or Safari). It works offline: the page cannot load or send anything, and your firmware file never leaves your computer.
2. Drop your official XDJ-700 firmware version 1.15 UPD file onto the page. When it says it is detected, press **build the mod**: the notice opens; read it, tick the box and press **build the mod** there. It takes a few seconds, checks the result, and saves a zip with two files, each named `XDJ700.UPD`:

   - `1_mod/XDJ700.UPD`: the mod
   - `2_stock_restore/XDJ700.UPD`: the restore file (the official firmware 1.15), to go back

3. The page then shows the steps: open the zip, copy the mod's `XDJ700.UPD` to the top folder of the USB stick, eject the stick and plug it back in, and drop the file from the stick onto the page, which tells you whether it is exactly the tested build.
4. With the player switched off, put the stick in. Hold **IN** and **RELOOP/EXIT**, switch the player on, and keep holding until the update screen appears.
5. Wait until the update has finished and the player has restarted. Do not switch it off before that.

The patcher refuses any file that is not the official XDJ-700 firmware version 1.15 UPD file, and it only hands out the mod if the result is, byte for byte, the build that was tested on the player (SHA-256 `84f7f0653a4c66cf5b902edf1d837989e443ab5ab3462aa0ffed8b6530076f56`).

The same patcher also comes as a Python script, `patcher/xdj700_plus_patcher.py` in this repository, for those who prefer the command line (Python 3.8 or later; on Windows the command is usually `python`). It does the same, with the same notice and the same checks, and writes the two files into the folder `xdj700_plus` in place of a zip. It then shows the same steps and checks the file on the stick: drop it into the terminal window and press enter. To check a file on the stick later:

    python3 patcher/xdj700_plus_patcher.py /path/to/XDJ700.UPD
    python3 patcher/xdj700_plus_patcher.py --check /path/to/the/stick/XDJ700.UPD

## Going back to stock

Do steps 3 to 5 with `2_stock_restore/XDJ700.UPD`. Both files have the same name, so keep one at a time on the stick. The update screen shows 1.16 for the mod and 1.17 for the restore file: those numbers are only there so that the player accepts the file. The mod never touches the player's boot and recovery region or its panel firmware.

## Problems

Found a bug, or a file that does not play? Open an [issue](https://github.com/lowtrustsociety/XDJ-700-PLUS/issues): say which file (format, bit depth, rate), what you did, and what happened.

## Building from source

Not needed to use the mod. See [docs/HOW_IT_WORKS.md](docs/HOW_IT_WORKS.md) for what the code does and how to build and test it.

## Support

XDJ-700 PLUS was developed over the course of two months: starting from the official 1.15, it went through more than 70 builds, many of them tested on the player itself, in sessions of up to 18 hours, and a lot of time and tokens along the way. If it is useful to you, you can support it here:

[ko-fi.com/lowtrustsociety](https://ko-fi.com/lowtrustsociety)

## Licence

Copyright (C) 2026 low trust society.

Free software under the **GNU General Public License, version 3** ([LICENSE](LICENSE)). You may use, study, change and share it; if you share it, changed or not, it must stay under the same licence, with its source.

Not covered, because they are not ours:

- **Pioneer's firmware.** It is not in this repository and the licence gives no rights to it.
- **dr_flac** (`src/dr_flac.h`) by David Reid, the FLAC decoder the mod is built on. It keeps its own terms (public domain or MIT No Attribution), stated at the end of that file.
