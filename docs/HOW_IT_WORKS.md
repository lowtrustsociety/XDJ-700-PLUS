# How it works

One page for people who want to read or build the code. To use the mod you only need the patcher (see the [README](../README.md)).

The mod is meant to complement the stock player, not to change it. What the stock firmware already plays still goes through Pioneer's own code, untouched, and the way you use the player stays the same. Our code takes over only the files the stock player refuses; every other change is small and listed below.

## The player

| Part | Chip | What it does |
| :--- | :--- | :--- |
| Main processor | Renesas SH-4A | runs the player's one application: files, screen, LINK, the front panel's exchanges |
| Audio processor | Texas Instruments C6000 | decodes and mixes the sound; its program is part of the application and is sent to it at start-up |
| Front panel | its own chip | buttons, jog and lights; its firmware is a separate part of the update file |

The official update file carries the application packed (LZSS), as S-records, together with a small boot and recovery region and the panel's firmware. The stock application plays MP3, AAC, WAV and AIFF. It knows the `.flac` extension but has no decoder for it.

## What the mod changes

Our code is added after the end of the application, and the application is changed in a few small places so that it reaches that code:

1. **A jump at the playback dispatcher.** When a track is loaded, FLAC files go to our adapter, and so do the WAV and AIFF files the stock readers would refuse (88.2 and 96 kHz, and WAV files with the extensible header). Everything else goes on to the stock code untouched.
2. **The adapter** (`src/flac_adapter.c`) opens the file and describes it to the player as plain stereo PCM, the way the stock WAV reader would. From then on the player treats it as a WAV file. A mono file is handed out as stereo, each sample on both sides, so mono and stereo files play the same way everywhere below.
3. **19 redirected calls in the input task.** The player's reads, seeks and end-of-file tests for PCM tracks pass through small trampolines (`src/pcm_route_trampolines.S`). For one of our tracks they are answered by our code, which decodes on demand; for any other track they go to the stock routine.
4. **FLAC on the audio processor: the feed.** As Pioneer's bigger players do with their formats, FLAC is decoded on the audio processor. The track still travels the player's PCM path, declared as a 16-bit stereo stream, but that stream carries no sound: the adapter hands out small tags in place of audio, and before that copies the FLAC file's own bytes into a ring in the audio processor's free memory (`src/chip_flac_feed.c`). The 16 bits are only the size of the slots the tags travel in; the sound itself never passes through them. The reads that feed the ring run on a stack of their own (`src/fed_stack.S`).
5. **The loader.** At the first FLAC load after power-on, `src/chip_flac_loader.c` writes our block into that memory: dr_flac compiled for the audio processor, with a converter of ours (`src/chip_flac.c`, `src/chip_flac_glue.S`). It reads the block back, and only then points the 16-bit stereo entry of the stock table of PCM converters, which lives in memory, at our converter. No instruction of the audio processor's program changes.
6. **The converter.** When a tag arrives, it decodes the FLAC bytes the tag points to, at the file's full depth (16 or 24 bit, mono or stereo), and writes the samples straight where the stock converter would have written them. For 88.2 and 96 kHz it decodes two frames of the file for each tag and halves them with the filter of point 9. Everything that is not a tag it leaves to the stock converter. The decoder there is built for that processor: the prediction in a pass of its own, the leading zeros counted by its own instruction, and only the frame header's checksum checked (the bytes come from our own memory). While it runs, its part of the memory is made cacheable, and written back and switched off again before it returns.
7. **When it cannot keep up.** If the audio processor falls too far behind, or a stream's bytes are not there, it gives that stream up and asks the main processor for a new one from the same place, as a jump does: a short gap, not a lasting silence. If the audio processor's part is not in place at all, the adapter decodes on the main processor instead (point 8).
8. **Hi-res WAV and AIFF** are read and converted on the main processor, and so is FLAC when the audio processor's part is not in place, decoded there by dr_flac (`src/dr_flac.h`) with a seek of our own that never decodes more than one FLAC frame to land on a position. Up to six tracks keep a live decoder, which is what hot cues across several tracks need.
9. **88.2 and 96 kHz** sources are halved to 44.1 and 48 kHz while playing, through a 47-tap half-band low-pass filter. The player's audio processor does not run at the higher rates.
10. **Pacing.** The player keeps about 30 seconds of audio ahead and tops it up in bursts. Decoding in such a burst starves the task that draws the screen, so our code spreads the work out, a little faster than playing speed.
11. **One small patch** makes the player ignore the "unplayable" mark that another player may have set on a track.
12. **The version label.** The two routines that put the version on the screen (UTILITY's VERSION No. and the start-up screen) are pointed at `src/version_label.c`, which says PLUS V0.90, and the two stock texts that put "Ver " before it are emptied. The version field itself stays 1.15, because the update check and other devices over LINK read it.
13. **The panel guard.** The main processor talks to the front panel (buttons, jog, lights) in short exchanges over a serial port, fed by the chip's transfer controller. Now and then, over a long session, an exchange hangs: the port's receiver overflows and stops, and the exchange never finishes (most likely because another transfer held the controller at the wrong moment). The 1 ms timer interrupt's handler address is pointed at `src/panel_guard.c`, which calls the stock handler first and then looks at the exchange. If it has stood still for 200 ms with the receiver's overflow flag set, or for 2 s without it, the guard does what the end of a normal exchange does (clears the port's flags, stops both transfers, frees the exchange) and the panel carries on. A healthy exchange takes about 3 ms, so the guard never acts on one, and it does not touch the sound.

## What the mod never touches

The first 256 KB of the main image (boot code and emergency updater) and the whole front panel firmware stay byte for byte as Pioneer made them. The build tool and the patcher both refuse to write a file otherwise. The audio processor's program inside the application is not changed either. The player always reports version 1.15, so a restore file tagged higher always flashes.

## Layout

| Folder | What is in it |
| :--- | :--- |
| `build.py` | Builds the mod into `build/XDJ700.UPD` |
| `firmware/` | Where your official 1.15 file goes (`firmware/README.md`) |
| `patcher/` | The end-user patcher, as a web page and as a Python file, both generated by `tools/make_patcher.py` |
| `src/` | The mod's code for the player, and the host test runners |
| `tools/` | The builder, the update file reader and writer, the packer, the host checks |
| `tests/` | The test suite |
| `toolchains/` | How to build the two cross compilers |

## Building

You need your own official XDJ-700 firmware version 1.15 UPD file, which is not in this repository (`firmware/README.md`), the two cross compilers (`toolchains/README.md`), and on the machine `cc`, `python3`, and for the tests `sox`, `flac`, `ffmpeg` and Node.

```sh
python3 build.py                                 # build/XDJ700.UPD, checked, with a report
python3 -m unittest discover -s tests            # the tests; a few skip when a tool is missing
python3 tools/make_patcher.py                    # the two patchers, from that build
```

The build is deterministic: with the compilers made by `toolchains/`, `build.py` gives SHA-256 `84f7f0653a4c66cf5b902edf1d837989e443ab5ab3462aa0ffed8b6530076f56`, the build that was tested on the player and the one the patcher writes. `tools/build_stock_restore.py` makes the restore file. `python3 build.py --everyday` builds the mod as it was before PLUS V0.90, with all decoding on the main processor and without the version label; it needs no compiler for the audio processor.

The web page's patcher (`tools/web_patcher_core.js`) is tested against the Python one with Node, on a made-up update file of the same layout. The tests run the same adapter, feed, converter and decoder code on the computer against a model of the player's file calls and of the audio processor's memory, and compare its output with reference decodes.

## Pioneer's material

The work is based on Pioneer's official XDJ-700 firmware 1.15. This repository holds none of it: no update file, no built firmware, no listings and no excerpts of its code. Where a tool has to make sure the stock code is what it expects, it compares hashes of short windows. Addresses in the source are positions in that firmware, found by reading it.
