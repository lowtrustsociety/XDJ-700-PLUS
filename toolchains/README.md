# The two cross compilers

| Chip | Target | Installed in |
|---|---|---|
| Main processor, Renesas SH-4A | `sh-elf` | `toolchains/installed` |
| Audio processor, TI C6000 | `tic6x-elf` | `toolchains/installed/c6x` |

Both from the same sources: binutils 2.43 and GCC 14.2.0, C only, bare metal. The scripts download the two archives from ftp.gnu.org and check them against the GNU signing keys. They need `curl`, `gpg` and a C compiler (on a Mac, the Xcode command line tools).

1. `toolchains/build_sh.sh`: builds `sh-elf`, in about 20 minutes.
2. `toolchains/build_c6x.sh`: builds `tic6x-elf`, in about as long. Only the release needs it; `--everyday` does not.

`XDJ_TOOLCHAIN` sets another place to install them. The compiler for the audio processor cannot be configured in a path with spaces, so it is built in a temporary folder (`XDJ_BUILD_TMP`, or one made under `TMPDIR`) and copied over at the end. `toolchains/installed` stays out of git.

With these compilers `python3 build.py` gives the tested build byte for byte (its SHA-256 is in `docs/HOW_IT_WORKS.md`). If it does not, do not put the result on a player.
