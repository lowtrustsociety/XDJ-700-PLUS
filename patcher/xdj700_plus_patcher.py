#!/usr/bin/env python3
"""XDJ-700 PLUS patcher.

Adds FLAC, hi-res WAV and AIFF and Bandcamp WAV playback to a Pioneer DJ
XDJ-700 by patching YOUR OWN copy of the official firmware 1.15.

    python3 xdj700_plus_patcher.py XDJ700.UPD

The argument is your own official XDJ-700 firmware version 1.15 UPD file.
Two files are written, each named XDJ700.UPD in a folder of its own: the
mod, and the restore file, which puts the official firmware 1.15 back.

This file contains no Pioneer code. It holds only the mod's own code and the
places where it goes, and it refuses any file that is not the official
XDJ-700 firmware version 1.15 UPD file. It needs Python 3.8 or later and
nothing else.

UNOFFICIAL AND EXPERIMENTAL. FLASHING FIRMWARE IS AT YOUR OWN RISK.
It shows the full notice before it builds; the README has it too.

Copyright (C) 2026 low trust society

This program is free software: you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation, version 3.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
details: <https://www.gnu.org/licenses/gpl-3.0.html>. The source of the mod's
code that this file carries in compiled form is in the project's repository,
under the same licence. It includes dr_flac by David Reid (public domain or
MIT No Attribution).
"""

# SPDX-License-Identifier: GPL-3.0-only

from __future__ import annotations

import argparse
import base64
import binascii
import hashlib
import io
import os
import re
import sys
import textwrap
import zipfile
import zlib

# The mod's own code and where it goes. Filled in by tools/make_patcher.py.
PATCH_DATA = {
    "build": "PLUS V0.90",
    "date": "2026-10-30",
    "known_issues": ["88.2 and 96 kHz files are downsampled to 44.1 and 48 kHz respectively.", "Right after loading a FLAC or hi-res file, the screen can be slower than usual for a few moments while the player buffers. The sound is not affected: no delay, no lag, no gaps. It is only visual.", "An edge case with 96 kHz 24-bit FLAC pitched above +50 % with MASTER TEMPO on: spinning the jog wheel very fast in vinyl mode can give short gaps in the sound. The sound recovers as soon as you stop spinning. In extreme cases it can end in the player's emergency loop; loading the same or another track recovers it.", "FLAC files encoded with very large blocks (16,384 frames, which is rare): the same applies to very fast jog spinning, and hitting their hot cues in a frenzy can also give short gaps, so better avoid that. No emergency loop has been seen with these files, and in normal DJ use they have played flawlessly.", "ALAC, 32-bit, 176.4 kHz and 192 kHz files are not supported.", "The update check, other players, rekordbox and the error log still see version 1.15, for compatibility."],
    "mod_tag": "1.16",
    "restore_tag": "1.17",
    "stock_sha256": "73edec9802da51672257c2599efc04209dc92478fcbaa1a0425b3b122e33f99c",
    "stock_application_sha256": "1875381b56d065a2b0a97a63b64ead5ce71397c521b7a62713c5bb4a0e055939",
    "mod_application_sha256": "8d47990ab12dde953c068eb5fed6361187ded625febc58b7996027da6ae7dbbd",
    "mod_sha256": "84f7f0653a4c66cf5b902edf1d837989e443ab5ab3462aa0ffed8b6530076f56",
    "restore_sha256": "d71006018c8e2889be5517b0b1c853cea92b6373196c8c2f7040c25c822b0a09",
    "changes": [[755464, "08781c09"], [877204, "257300000000"], [877220, "00000000"], [12010774, "19441844450a0900"], [12542496, "0cd81b0988d81b09"], [12542940, "0cd81b09"], [12543660, "88d81b09"], [12544296, "0cd81b09"], [12546568, "34d81b09"], [12547848, "0cd81b0988d81b09"], [12549160, "5cd81b0988d81b09"], [12551216, "0cd81b09"], [12551836, "0cd81b09"], [12554036, "0cd81b09"], [12554656, "0cd81b09"], [12558104, "34d81b09"], [12558784, "34d81b09"], [12560092, "5cd81b09"], [12560748, "0cd81b09"], [12561460, "0cd81b09"], [12590712, "01d02b400900"], [12590720, "88d71b09"], [14490244, "70771c09"], [14495724, "70771c09"]],
    "tail": (
        "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"
        "YXnRekODyNMLDfevGnVuaZdPucLVumGyJu229kVvaYavo1hatEOTtrB9cFPePb9svTdyrexAUHP2E7tSLl5xxLfcbx+sLe4q2hFd"
        "6vOK1pg1Va9Jm9X+y9b1+2I2KXdC6Ql+4qx+wNKyx6ZJM7b7KgbFOoq2a9JS22/YHzYlB6477re3Oie4v7M6TbnOqrTYtKJjp0VT"
        "row5NzqQZJZ7Es0j/Rnu/rXXHNcbjCZRX01OPiidPeZSvbguKP/5qj2uVB2MMqS6dbkO/0Vb1NmkdwZrk87IL5l2iPAvf0crwh94"
        "dtOgO7NhYmvMhv1Z3wgU9HW45szQ5wz985hHuU72ozKfd3raO6PNO7TLlStdB9wzA0c2ThLln9oQ2zm27TWdUrNX7nSiUklRt67p"
        "Va55Sw6ImsKywVrNsv+cfmTT2D8f83xUpnyT5bNv0ohvMrRO7R0yS8bRPTGeRLPs/9kT/faHupS2mzEn3ttxWmm5DXjU7gxXqnrA"
        "Xlyt3L8lxpWbuzmofLs+J/LtGw+5PrJOTxvckvftd2LVZ7XL+2qV77898M7m8JaPtijjvfa5Wk5mf3Z/l9v8mqUiZEt93p/N3P3W"
        "td53Np9yZ5eO6o13Zc36MO1ITP6OnV2yqDe/XK9JKwqoNw268rbKrZo0c3CwRrNisPadknc2+7wvzfy455Qr1TroPiK9U5LZ8FZa"
        "anPujo1dN/di8OadWnr+qzNaSpwdFRjV8x+PXt81vfPxpYmzVcGRbUc6r+7SfSD5NR/cGbnqvGDHrV9BpopYGNyYefcR0ab56d7Y"
        "jR2iNTIyEO865LF3J4tYTJVT3T9LT3Wt6JbvTpWU/ZfaPGjTinaK7H+0U/fBsm6NdZKclK4ODDgLHGmmJf75XTePWmuBOGpLRWt/"
        "cFPmPc+K7x/eOlXkypx8/4DdsDfWdTA3N3jRcX21ciXOjcq3O5u8o0amqLbnjpZTorJNA2sfceothuB+R6JFuc/LQnOyXyNJHx3a"
        "cNvsScUPGf5giQu+YHjBIgceNMf4L67dn6UKdOb+Z2eTe1Tgsv2STWeR/JJo+/yys98u9Ujn+h0XbXqjW6+MQSid77cfcg04+p2N"
        "cVPsRS1Tw0NSfLzFrWvQizaqUvOKbz7mSmjWNVy0LYo/HK/cYf7r6lcjHl0aipbUj24LDdqsKuUa1C/eg+eL1zIrbU/lfjExbcqe"
        "UK71H+EvuVnPrQ702+93XLHPqE/bnXzHg8U5wRlbHqs+VXejp0S5W0HTm+lzq0tKD5gfXzRQOei6oyNza0xrvWVq/BXHLn2M5+Ze"
        "q04un2PMdmWJ4y/bkyL+m3oyWxr0ZDYq92WNacsKjBFrHv2P38hTQimyIdLLbczRpGUGBjzhxoza+x0HCiK9iAUTAlfsN/r+Up+8"
        "elONxf1K6R3l491vKndX2L4r40Lp7TWbxZK56fXm46HNi5Ieez30GzkrVO22hfod/Xb57OgujZTokM6JNpB3Vr7WOqlA6TkIPnX/"
        "KunsHSGn67j0vjrSNybi5tWVt+6t8PnYc02iBj+5y7hy0Q88NdK5K7drrd/L06SNDh6qVcaR/mrOndr1YM2xOmVbZJEWLgTUG3/t"
        "eNCtM7/v/80d++y6kveCx2oSunwbcnclRcaI1t+M8ZEB6Zyuu+qscsfnUyHpQ9+G3bt+Yz+xaUTyG3O1XQnJw3eFTkr/Y1Ca+wf/"
        "H5OfD41LPi4p4a23DdcnlPaH0qZS6hnyzTHdlWvnlN6oUYGq6nK3ctevSTfv+lXltniSysrrppUWmycYq92i5lw6bUFqYXRp0gK5"
        "4XZjeY2uVO1X9za5+0JZU6K2K3cJuxTK0ckNFtEi+1fjm6GcSB+VNzw8RpVzw/AYVU3rb7U0yg2qwPWYNZNl/6FO6YM1J9ec+dlD"
        "a97Vlp3y/vsPVb2ZE095rI2D9h36tPN36aU+5+v6mfsmHpxombh34ncm6vT79Af1Pv3s8Xn6fP3c8cXjB+2Dm7Lv8XmjzmlLj9VE"
        "te0MRp17x3lG1Jh/4dpikc4XT/y2Oca1pG1pcObEp1vnt633P9NqaasJDjo094w8dywy6svItjuCI8893fqOqImfccRsKuy69cnB"
        "mpkTo84VbFfGPsrdNmQzdWs8WZ5pnozq6HK5PKMuetEHU5Mzsqz3ZCz9YbL1vj/LZdqMY65dRt2Lc41S++jGbENGjTFHPqddouzL"
        "6eIIfme1dO6Mc1bhFsvuiZ2hkom1bQb/063L21JFuNxtyf5jreVtiUHpfFLXYIUyMpBSro8JvJidFZB6/+B+/vY/3HmiKsGgjK/9"
        "guH4jIzg+7PS/Ur5+nVpvk0nRTV/Q9n3yvWU0nMG6aPC4esmJSk8dufE1OiyU+Xvlr+daSgwlJ8qEPmc2jP2ReWeEFK73JhbPTLc"
        "uK+4+u9VszJezE5RxZZlm7IMyvgkk56Q+qTew9OlvpmGOcF5BqknxSCW9BSKFSQZpl+QPhx0RJ1T7niuLcvdHtV2NHijd8j2iVNT"
        "1/zi9O5dddM9O+uKMu77syawK/1T5/XKtyw/K1g/RdRVTY+0R50TOWfflhJ17bw513dE9RUUZLar/lRfsFmdoY6W8qTfyXOkndJx"
        "WaWM47ZEEuXmcwElBYu6wtK87Td6NGkpbp9XhEfE+fSzvwxJfWNOqTqLpo7p0ky1tos0P3WCkjf3TSqZN2ea8eeBQUe6KI2y46Kl"
        "xSJe5ovJOnr4+lKlT2JITFfF9ExHgSndrzfEf2FvDtjfCg54xHf1TiqZNaei+5JjXHFCz+iY6OXjY1LkVPNkgzLa2sVVgc73Z2mX"
        "XapQ+svUG7JC0gdLu/0PLX1DW6r2tj3oq1ncZd1+wNz68GSD1prh9TnudxqzvuO4aE9PVu7BP2VvQXJH9b+16JrSWi2G4ub01pLm"
        "dsd3ndOSWx2zk6cnb+i47NAnH7BMTn6u96o9K+NYcofnveTdjXP1Semv6dXBf9dfsSel69srH5XOfWw/MOcvyZ22VEux/mDFlVXD"
        "+apKHJWPdHZ4YvTa5O8rY/oGOzZmdOXenXFayYkOeY1nMt7RrlBGqFIFD7S1da4PqwKHar/dedEx6LhUMbNVu1zpDbkQvOgsMVy2"
        "W8O/MwzW/I8hb3vi2MTIfep/cVT6IOFUsSh9XjfE15eEDhuUI0U5TraI/Ox/2i9WSu9pb7u06lshTZpf/kjUnnJ1U+OelWZ/v6/o"
        "1rGr5GPKdcBKXqb0qYRTxCT2y12xOTb9uexVKWpjToOhWOTiZoO+bcAz4DQE+1dnV8r31lvMtaZV62uL9q6p2VC7tmZ97Zoad7wj"
        "Q+MyV1qU5S2fL6+Od8Q/V/Jq8GBFVGenTeor1s8VsdXh2d14sGLKquFxdOX2zaHv2y862kMzDdKFhPPnQrsbr6umGce2P2y/LumN"
        "+x0d7kecuoZ0nbXXrBsyp7ZOd2Q3T2yd2TzzaIfrOznWm/liufvz2nanyBPXGTYZRgV2jP1y74vxkBTWBAdtXZHeF6013jvSkNKZ"
        "alDura/bqlsd27rdEOOZUW2MS2uSwnkiZkcaUg2DSs+LY5qxMDjLcEKUI4ORnpdBm9LTL7fFBYfWWcOXHdfVl+3Z5uujsi2aUrls"
        "t0lu1JWpAkMN/zh4vWlo3eHp6uBlUQ/XBobsx25+h9r/9EmjW27QG4fsl22ur+3F0Z77qdKPszFm/WHxqYptFb3XnUcj/TjX1mXL"
        "0nuPbzCJ+neZwWA4PH1EsGj2J1WiXuR/3nDQcMOm9OQkBjTWG3bp/UGnprR/7YqmVFOqZXCVxX3J4atbsX2wYo0punR3oVyaXWjb"
        "dv9BuWHwzknv9dut337PEV16zhljnmaIsQw4fK7Zhui2FWWq4Iz0xzsvRHp0NG3RT1ysGnRcaL14p60xxhUzTxWY3Ko6eLHq4h0x"
        "rquiZfnx6piSSw5daWnwamW/Y6jn41VZ6ZdWx4ujSVOWXZCdv9s8UFGQ5w2Y81T+/tWJhd9s77drpMu2Icek3AHnSLEF88zZBo00"
        "Xrrk6K9IkT9xXluXahqoUvKCpZZBx0tz5ohXdzf+3JBpn+y8zfRXS4IoxQ5PH3Rct1sbF0f6c9JDg2s/sU9V+nPWN264sNhUcWaJ"
        "z7U/t2HtBaU/Z0n5piyrpXL/3Ma1TVUXFqS3lLuyrM6NZ5YYW46J//bPyWhRfjG/sDCz5czyQ+K/6g2TW16crZxpMKUla3GHmLcY"
        "dU1TW3R3fTeka6yt00iVXvnDC4vOzG9yLc7NWpRlbTAeNaW3SB/91XJ/KKpRblBi5+KdMSWDtqsVtq0frxnResmhxJ6udHKwf91Q"
        "z9XKj1f9NP1SlYin1cPx9LKIpx15IwONeSrxjsQ5cvumUL+9tlqsy3Wm/II1xpJlzSpPb5HDJuMYk9Q3r/CcWNOe+pbbSysvLN+z"
        "7sKystVnFj5bcEKE1re2rObC0nLnmUXpLcpWGFuOFCpbemFJRsuZcuV3abGlCw5YlG2d3PJyvmV4SxcoW2o36raJLd35U7Gl0dKt"
        "fpaLIu9Qxqm61d/yYvY3g4vDg4754aurxDFQYWu8VDll7HjDFbuu1BMYb7jQWrYppt5RNz9sd14f/cqisrWvlP+ktXRdjMvhiF1W"
        "vuaYWy6N9Acp8yWZ5R1i3rHZaX+lLHZxZpnP9UrpoXqln+WYa3fu8cLi4nRr+qL05VnWVwxZi+QpTa7DFndLevmntuySrHJfvdiH"
        "Zk9L+gqLK72suiW1+JSrzSRiq8zbkr48w5VeWtOSVarES22Lbnu8S7etruVnId2OwXXX1XLD1VW2uz+umLv+UqUtbNj55NiJhk/F"
        "NswOTjQoR7Oyn4aPZ5GWywbXZBcMOLPzbdt2m607B1YXFI4MmAtv7a+Oo/32BUqPUV2Mq3ZD7KJAa4xoczVteMoqL7m4LtPatCFm"
        "9s5csSVLlD6jLOtTS9Jbspb81bTPKIXHWF4Xe/NWrNnCpeuuy68ssjvHtZbbXymPcZWtjV0mYq702Obxhi/H3RXbcOwpcZdZpsRb"
        "cbFsDSzvcP0hJzvX+tFQccAqp2aWXrHtLjlemJobsKpTA4uu2LIWHbZoTE0ud8uAK1CeVZ5l9bRYXPvN1S3P2wJlWWVZVm9Lhqum"
        "JbDihC1QmrUiy9o9Jav08xjcJGIwxnN1tdj7lS+bLzl1pcbgxxX9TiXOZisxViNygAVVBVWFy/OVHCBK5ADOSEzZ/LVe5agWKWj+"
        "TGvW/Cxr6dSjpoZI6vkgFHVX42ZPfcPtF1a4181cfkrstca1F5Y1Vcxc6llzYUn1qgvz5QxlX5oiadfcohzDlpaZkVSa1+ITj/kt"
        "MxcoKbwgkqoLRarV7QiFdE1lHs02W61m+yxVfe3LBZvLsjw5nvSpqrt0pf3Bi1U37LolkR6oIs9Tg6ueeqrfPtLQd0TqMbniWmKe"
        "ktOVfqhbv0d8fu9GZQTZi/eVGeyGZ4o0wf6qXxctDPjc/Xb97DJlXnVs86PF3y15Y6bK79innGe08oB0YWrXiZqpryu/uw5UKGPX"
        "qTdG7qRhi68znj5Uq5Q3Md60yLiGyuNAReSKBvek1zx18l2NU0aeTzr576MmvN5XmxJ1V1TKKPku3aLxQWUUpUlFiSUjA8vn6Uv+"
        "I/hM0ZPFvxaPPneHK+ZrypwpNU5XslWaosSQuzlyJpPV0/xmSL1DktcsFWm9fXr0j8s/T/fK7/+qbvmhGaXR0jZ5+PfXL8bHcMuu"
        "evO5HKeIkY/uu27/9HalxEow/GH2WP8Ls1WBDvfJotF+n+vr7o0Ydp+obng01T3BM3PpKc93zY9aijLUwYOidX9qY9scy6lDnoll"
        "lteVVsFAhbZcOicb4utEfBl0W2d1x7T2V2af1pbGeAcjowINePqdRRlxgVzztC5jzmfx55lyyqyUwhnKu/4r2OHud+aafyPmGe6M"
        "ulmmCad2F+6cevWc5Jd7dKUTghk1E7r7lPG0th2foQtopIviO1NEa/NQ5NeJca/He6LL5MZjyr2g/alzRX2252hRe2jS7II6S81z"
        "2+sXvRO6b9yOWk/GP0NRTaO7LlYqdcJ/hNSbJBF3Xxe/6yekRiv34DkQuT/LmskP59wIP5kz2vVrUed5IUfXMHyugnHlkfvGNZfa"
        "KgzS2Q2itl5WobXebdCkxQZjzok6j+GqreJrYveg62CkVzB7e0zbP4Ky4aL9V6eT0qX2X57+1ZuijbYke5sm7e8BjeGi45nT00QL"
        "7OnTytI8sXRdUL1e2fKuwKD3qdPDy14KSOel8F2i1qQ2pPXqDTf6phikf+SLVox04QmbdK5f1PkOijj/dUBz7vM1a0R7Rvmm/d3f"
        "CAytVe6q/KjYZ9nb9m/QpD2yYcC9f4MqoN7yl87EnoeUtZcqYUoWS5T7Ku4Lfvmbtgav2Ts2X713+Jwjfcn6YL8jqaTPoGtMNWek"
        "VIiWSVrvFKVtFS403BChlHrHiRAaIyF8KiS9r6x99+nhdQy4Rfrb0to56J3W07E+cbbkn/b+WLHOeUFf7efrbM4t8A+6ErbG7I3d"
        "9K0uae6o4FTxnqig0uel2XveELtp0J3XEL930JPQOLQmtk5dPXNvbL3ao5w7kBxsNJyou3HXrwsTAvOM+r0pcqxLuaesLjfGf8ep"
        "gtly+9ud/Y5Bm9w3Z4b8knaGaP8EBlYnhaTwlfCuNF2b3V/RrSvStiuPmoDSLlnc7hD/q9rt4rHMf8jxsdNsjgpY3NNMiSWSP37z"
        "gONl1yG3riG6LLFZLhtjTIla7i13JTQv7FJ6sy/ZE3PFu1yfOKx9USlDlj3GHztK7AOrjenz7O8e3Zo+72bM/ELU0zo2qw2ahp+H"
        "4lOG4ybx/Tix3ZqvxI0qMOh+7qgUTmxI3Su9H+uZuHeadUZawYMa96BL2UMJW8cZxu5VYijOrXHFuia600wxQZVh0FWwVXbLeye6"
        "p1knpGnWt69vW98fjl08WJ+w++W9co+m/pX6E5HR/BYFxon1Lgz2bdK8PehNPK1dNm+b0jMghZXtHNe8LtS3yTo2VdRNEsTUJdJQ"
        "tphnKveHFFOxmLaJab6Yrt+QJGUsm6+O566cl7RcqduIKUk5AiNrOOlP/Oz/l/0Jn/3/gn/xUWVMo+s98q5jLk3jI+nZpsuOOIN8"
        "ftCuK5NfmmK4O10V0KYr45fqQ4O2ftvI7sRcuf1bR5+wzRCfjJaqxHq8YqpvGx5b5/NxhXV3D5cs1ZF89MnW6/ZSUfd3ipw0xv3j"
        "1sacmI3xLlWbyKeWDjpzPUrfarzrw07pfVXbrID0waF66f2Xiwfc+tlR/sTZ2SLX7S3Kah/w9Lm/a3rUPM04y//rB6eYZrT3bWqb"
        "e3vXsZrbX1N+Yx41eu0Z6bzanaK2GD51HrEM2nbkZgU/XWXxNHkttWvM7+SldMqNK7v6aj6tVN6vXt/h/vqcvO2xU6LNOt8lTZa3"
        "S1PkHbqyq/5PHUcKX7a8bD6ltP56JxXNmq2b+1T7u6EOd+NjXpFaT7lTTbNP6pqO1ee1vlEc4xol1z2QIo8szjZJfnV9QesrHstp"
        "6VyMS2XIb82oLsrQlMV4xfb36qoPudRtrmBxtb51cWZMXVxrjKsykGSdVq7NmDv1at+ALWlR8SJVW+NkeUfU9qjAoepql/yi1C71"
        "6crCwWLTpO4TNYMVVflKLv+pSFcjAvE1KZG8+oQ31vByyWtznw70O6We3xVNmj3gMc5WSuEpsw8Geos62/uUUsqUlP64X78v0fxo"
        "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"
        "Kx8s0vj/d6g6RG35NVGbPLXJ5z48XVN6wht9Wjn361BNar72tmO1o07K743qMrqfNYm8yijvezuUWiTJD2b/XnNk3/To45+t99b5"
        "V/O7lTXqq8tPGlemWbSiXnyzxvuV9eqrXsweCErhuHXx9k7TheAhT80+pX35fnt+V8Gryhlk+d3KGVVSOK87/5TevfrByBnoGbqy"
        "oWBUk+lN87tHZpvf1iwxnzGffDDH/IZ+ln6GfqbeOODOfTOpKHqx9eGcV5XfV3NOK/NDtQNecbQv8dXmP5wdWX7CJbfNeDPLP+ON"
        "GafFksXR0gzx2FujPD5TM108nqxJ7M56VV/Tb9cuTsp/RuydR0Voxrw5Rnw+WhojQrfPn3RaG/lsdGTZOLGsTdTJnwvJH933DUlu"
        "lqSoaOmN306PXjN/6Xxlf2k+2BO35r/Vz+nuGKNV4q9ZujdXicMv7kdJuvT8tu3Kb9wXn39ffFZSXXxeCl95/g1xTOaKeBfftTws"
        "X3z+PTG9L6aPxPQvMcldyhEaLWV/LzW63H6tNys9PuF6b7ldmX96cz48BrbSQogX+yddtMHUgVj39b7Z6XHGsSfTXR2R54kdyvOY"
        "N2MjWxUTefSJ+FL7R3frXPJfpPTobqk3Lf1Wy6L+RqbUe0Mnpt13iTA2S9etYjLLI358deEYbZFokyhpOloqfKPZta5tVFDsx2lx"
        "Sfqk+KThnKBFGanZv2fv6n3y1lhXYu5gcE+LHP5XqKV5Rnf2SV11dnf268XVOm9293Kv3p1s3GpWKyOwPRAjUm+LqCOq6ywu5c7B"
        "izNV7T5nmmV0+95IDUwt2lNNrr0tUjgj6c+hlpaJJ9NM6na5Qbn+rKUl5aSy/WPbJ4lW+H/6k4bHsd+b0B3r0uXKAXmrT3zuqPjU"
        "WPH6z/y6yOute2Mir/eL1/8n1NpiuK1L+pP0t1HPmV/TdD6SGq1sr1qSrs5cNkYrSZLGVX4ycn2AEj/7Rdwo+cTglhF/L3c5vdUi"
        "R22q7ftWJA62REvP/Wq6yFsSupW8Jf6N5EwlPS0u0nf7RA4Tvcy40jFX91a8SJlj3zlWq116omaoR/tq8rSO2hM1xwu+l3/I/Xle"
        "OKP08npn7YjTl0KH3NHSDdHsFUER09XnlXQ7vB4lX7buHN313YrR3Zr5y72a+Vprat537ZqTE4ySOSog9448OSNdMmWpMlw3x4oX"
        "x69VHIfii5xicgwfR9adSl6jWTr+zfHdR0Seo1maOueFnOpNh7yivKlXUlrzqkNe7ZKobW12kZNvm7jMZz9Wo12SlN8SWR4jliel"
        "jw50uDWn/zL5kPfLWxEltuEvk6Ml5bj/8rrlnuH7Zlt3GlcmJ33HnvpWStL99tVJ0tkJSc321KTkbv3aDo/caIybtu+RdM2SxKQD"
        "G8Z1a5akztYu7fA2r+rwapcqYerw3r9KhGqBz65dqoRJWT4cpuiAiL2T3xFLZky+eQ2dCNHIk5dD31mlXIeohOk5kT6/HC6pV/OG"
        "dFYjSjFjukhEcRlxmXEjRR4xOW7EaZ97xKtTVirfcuuzUeKzIj2XK+l7RJdx5YhIzjdC5CNZ0vD4pMP5vJJvKPmAzVXbtlykH3vr"
        "8rbFfkeru22B39la3lbmr2i978Vv+FeJlFTsrxSPs/2rW41tBf7hnkuzX9OTWJCjjAfd46wu9+S5dKWS8VpQbmwOl7vnN+saxrWW"
        "e6S+f/15XHNiY0JPrlFXOiRebXJtSkhqi+qR/CN7UhPWtca4b1daza4JaRqX1BPrmpQQm2DdNi5Bk5aUoNSVlSsU5d5yt7VhWpzU"
        "o5UmJSjnUt26x/lwn8lwabnM3nXfKsNOg3LV8nXt0c4Trvj6NcW1xZK/ut79pjuS5yhnj6k3izzKf+e7fXaRKv17O6UPqyKvKY8q"
        "w+3iUb2xUvk/RyoZcG8LDji+7yzK2BrwbbDuetiZK9p+3+18xPmAI81UGzw16YeO7DkPOZ40u/x+x9VG45wqkRu85xg3STkDSG6c"
        "YNgv9vuoQOaktF6RnuaImlJP0pwjc562ST3PeAfrJiVl3zVYq9SElJFz5rVfs+vnzPXPPTm9x1edV6cr3RqJ0WdsqvOFr30i6vbP"
        "eFVt2UHpw0/svV598jW7NTk+WepJSDZ1m97SlmrPZvfEJT9gz27Qz+qxyw1xkwxJcs+kJOVuyXMDUk/WzbZlvvj/sUrl/ORHVz1g"
        "T03+YeT/h1dJHz5Z3Oi/tPbEFv8dR2ZfvffhkrmBw/M0S686Trg1pYdNR8xSn3I11nj/CVfu15TARRln2h8VMfSW/4CInddF7GwN"
        "Peb4Wyg5OdfYF5IblePOJsog5TitEPM1YqpSnov840Mx5e1KjQ6L6YCYvm/fX3LKkJTuc48OWBv2nww60x1plij/OMcDZtn/Yud4"
        "e5Jd6p1u6NispKsZhrSeDo/Si53W2+GeZorf/E6JiONeJY5nReL41M04VrVfcxhL5PbHQ4fqfV8Tz/d9Fs9yu1+85xO7b7P13p2n"
        "9BOl89fs2rNmb/zEu7q3TbRObDBoS82G7J4BZ9zEjpouJdYzTEqsxw/H+rdE/u7z9q/eNdEnWnZT7fWnBu8oNp0r2W+S/H2b4w2v"
        "ifkpw6C97swBU6Jhmv2yc2J5hl3dm2TJNBQaPK9qV0RLyqMqRxy7K9RblMdow4BbPq/2iDTxoj2YPNVoWCOWNoilAw7JtDjYvzop"
        "fZmSJs9J5684lbvXSu9ZvDvzvrfK/8DqqEBFyqhFI1JiU0ZEr5BXxORXldSX7C5RrlDbXeKrs96VlL51tSrQ5O5zZ5sWnkySmmpT"
        "CqYUNFXPCyRZyk9OsUQFpN54T1n3TnOqeUqJ5FfGaB/qSZ7p2/Jgp/XbHZ4kc3F3ovl+f253R42y7b6Qz6NcVbI/mGD4VUjqmdI9"
        "LtIu6quVd7hMR5Sz1ES61lWGC0b64+xGU1RQ7snqSTSr2uWefwtNP/fxarnnsVBG7eSTL4R2mmY8sMY0Uuk1dvc64uuesWXnP+ON"
        "f+217UqLd8CtMsntzZ1Sz0iTuqYgoN7u22L9do89ekG4IjVPK/aEyjBgG+rtsceY4iapG7LSw45sk+rkEyG98X2dpBFlpTgmv3gc"
        "Vt2t1Jd0kbwyVxyjeWIqElOxmK6L8u/GjZEie1bmY6WhvqLX5B513dqMhEkFuRa3xiS9pw7IDaLVGtSVKr855gSbqjVpv/S/J7Zf"
        "7e9ZVVnyw8Ap94wzoh4i9p++5lzFjJOa+T2rIqXmiuy8D8T7Uv3nHQnhc/ZpsyefjC296hxc/Vh65msz0g95NGXFZuWX4TT/oP2y"
        "I3VOxtHJk+ot1oyXQvJdk5Jyjx6q/8gRtuvM7/qTb6b46PYPK+JPfb6G4rzu0HnHX0PRk5Jd8dVKv/nGDIsr2ZrerJwtkms0RubJ"
        "1oxmueGlUFTjcF3ii2XM8uEyqlmpiyjpdf2UtEC5e5MnrXFcmT5oXDnmde2ialEnUVcr83ivtltdqy3NcKndm9ItruGSaER3tUu0"
        "CfvU7ltLlDNune7Fe1Ojo76jtIvWTxnO54fbHlMCxpWbNqXdM27Z+GD1hnGR0ROraxK6fV51vba0w6XvVm8Qy0qPuR5LO+H+cksu"
        "uku0U5dX17xUHN6utAD/J3TCLepMX32X9MfW4XUrYfjiNSDGlTunlq+3Bp2eyoLqDbXB8urvvL6jQBVwuh/oFLU48b8cSDqrO7vn"
        "Ladrz+vapdXe3aN211UV3lOTtn3N5DHt3z6lLXe61BsyvJG5W8lxm1za9Miz6p2vx7s6amZ719ZqF01buMSrXZRcPSFDOm9xNbm/"
        "et50ZeFo0WqQzv/vV9zdiYVrgk1el5iL0PTp+r55usn7zW7tAotLuR6g2DRhQ9qu/VmrgzvMoiZ73lP0jvimt0JyeP0US1B+b/Wb"
        "Su1KveHAvsTZWaOUMcVWvtXhXdmthFT5/G7ThNq0HeunbPAvfyOjZn++fN55K9SLI6FerIS6KbI3vxyyph/uKMgaoVnwrVBVflF3"
        "YsEr/kLx+LdA0vlXQrrz3n35pxMLs9R53ZryYstXQ/vTwA5zzls5Z0Qty6Wuq/ZqF8S7tFMjzzZlRJ6Jva88q/YNv5ZxwtU0HKoV"
        "kVCtUEKVPzdFHq19JndUUF2vxH2Ha6RpcvAXxRl+i+vG1jUh6Xyi2EPVw3toqfjOmsgzj2/4WZ3ybMwbM6aqq5Vt3vXFba6bMPXu"
        "kLrG4soX36K0o29dv6scR0XfGz6Wpt2fGv2ueG4IR0+W/1Q1ImNEuWvLxvW1Xo/rzqp1ayorHLbbFi+cX2adVzzHnDsre/q0KWmZ"
        "Vq00Wjmn6LM/JU1qpTGuLy5TlmqlsV9Z9uU/raRM0ZExrKMlw5811gxp+Ox20agQr2isn4SGp4JOyZrXqbGaOpXlSdahkBRZPlM8"
        "nyGWTxPz3M4k8XqSeH3aZ6+nieWTxOsTxDytc5o1XUz/DCVZx4vn48RyvZjrxDxJLB8vJvnPWmmEVSP+lLUnRR6nffaYZI2OPMo3"
        "H6Mj6/rfYVdejxbrH54KOmUR9mgRNmV5sgibHFk+UzyfIZZPE/PczmTxuhKLWZ+9niaWTxKvTxDztM4sEfYsEfZkEU5ZhD1ahF0W"
        "YY8WYc8Sy7KsSsxkSdGRUEVH9kh0ZB8MPyZHtik58i7lURNZV0XeF/+0kvKYab01Kc+NK+9w2XL1wWRpiStr/rnfT1tw9vcVeSvS"
        "YjPDv1/i/bv4f8rKFWnRLuXdWQuksPLKO7/PtL4lpjfE9JqYlFw005os1eamtXtz/5//Yt1Se31uZumkycq3pqiMudNyx7TfmXuj"
        "QdsuJqkqd03uEndsukbUwMXSnovBrNLhY0iS9kdNjn4s9l9xyYnalN9OnJxenPnJlNVZK2e8nf3xzCdnhWc9OevjmW9nr5yxOuuT"
        "KcWZk9N/O1Gbkpz4r7jHYidH74+SpPXX3r5cM6D75/kPLp0t73373T++PfLNn7729KuTT406ua47rXtd96iTk089/epPXxv55h/f"
        "fvvd8t5LZ89/oPtnzcDbl9dfk1XqqBEjR2lUKpW0cKVjhaQ0lJYsXKjMS2/OV5dWLrj5vFyZl69cEXlf7eZtacrc697mVuZN37pr"
        "mzK/beVtkfdvu+tbTcp8wxZznjJ3Om+bL92knFege1olSf9USVaDKPzmSZKxUpKeu0O8eK/02fkGyrkHkibmhi4h6cb/i89J4nOR"
        "9YmIktQ3J9VXJvn/MknSiOaxzaOlCc2TpAQprtnSXCAVS7Obp0kzm6c2G6Xq5lqpXtrQvFO6t/mu5kbJJq1q/mbzOmlZ823SfKm0"
        "+UTzC9LL0p+b35DONL/efFL6l3Sp+UbzJ9I/mv8pvSf1iKrWT5pDzT+T/qv5d9IvpaebO5oflg5IjzS3SYHm+5v3SL6W78jfk9tb"
        "9suPt/yw5UH5F/KvWn7f8p/yz1uOyj+Wn2gJy2db/qflQ/lay3X5otzfcqrlNfkd+c2WF+VXWv7U8gfZKi9oWdmyVL695U65Qra3"
        "NLXskHfLu1rWy5tb6lo8ckbLFDlbzmopkktaClvMsk4e12JoSZG1LWPkKFluUftGqmPU0b4k9UTfeJ9enavO933DN0c9wzdLnalO"
        "94kM3vct30b1Pb5vq7epG3xOX6W6Sr3Wt1i9wrfIV6Z+Tv3fvr/5XlK/7XtX/aq62zfou6weUn/se199wXfe16s+5HtSfUT9U99x"
        "9W99v/E9o35A/ZDv33yPqv2+76tb1S2+ZtXePcE931U9tudHqh+o9u05tufXqmdV/7HnsOqpPf++56Cqb8851UeqD/ZcVX2658qe"
        "AVWX6vSev+95S/WXPX9V/VH1/J7yPQtVy1VL9qxR3bFn9R6Haqtq+5779tyt2rRni8qrcu9JU03ek7NnumrunnmqPJVpT/yeRFWq"
        "KnmPRhW7Z9QekTBGjYlOnaAfV/SNAlNmxvSZW+/69q76TZ6axSsXllY4190RPv/PD4auDVx64++vdf/xxEuvPPDDHz3yPf+evYdD"
        "Tz7xy2P/+ds/Pffnv779zumTN672X+5778KHv/nFf/zu50d+cjDY1vKdjh88/ujUtKxZc4vzzYakuER1VOzo1ba1dy5bvqDsW+vd"
        "tU3b7r27/fv3Nz/04GMHnv71748/9bNDP/74+pV/ne39x0fPv/C3F8+8derVDVvqXNsb79ltr/zmmhVLy+cnTxqvG6HSxqRPyZlW"
        "MseSd9sS6yLHqqrbdzTsvG/jZm/1vNm5hcbJ2TNGypqxKRMT4o/+tPPfn/nVs//18L79//bdQKvv3Te7Xv/Df7/8l3M97//PJ59e"
        "HLQtX+VMq8ydNTtXSRAqlVqIumnEV4z8vxj1/5Hm/2cSAAAAAHyF/JXnSj/DDaUj8yalH+JT8fzW+5T+iU/E86ib71tHFAIAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAD/h733AY+iOvfHz5yZnczszu7sEqAR0N0E0F0IEhCV0gRmA9qg"
        "qSJyvSFYbwy7NCL3Sm+D3aC3LgE1sell16KlijKbgG4wyMZCS72JbEKgEahSq20CVmdjAgGhhjZgAoH9vedMgsH67/6+v+9z7/N7"
        "5n0eHpadmXPeP5/33zlnFoMMMsgggwwyyCCDDDLIIIMMMsgggwwyyCCDDDLIIIMMMsgggwwyyCCDDDLIIIMMMsgggwwyyCCDDDLI"
        "IIMMMsgggwwyyCCDDDLIIIMMMsgggwwyyCCDDDLIIIMMMsgggwwyyCCDDDLIIIMMMsgggwwyyCCDDDLIIIMMMsgggwwyyCCDDDLI"
        "IIMMMsgggwwyyCCDDDLIIIMMMsgggwwyyCCDDDLIIIMMMsgggwwyyCCDDDLIIIMMMsgggwwyyCCDDDLIIIMMMsgggwwyyCCDDDLI"
        "IIMMMsgggwwyyCCDDDLIIIMMMsgggwwyyCCDDDLIIIMMMsgggwwyyCCDDDLIIIMMMsgggwwyyCCDDDLIIIMMMsgggwwyyCCDDDLI"
        "IIMMMsgggwwyyCCDDDLIIIMMMsgggwwyyCCDDDLIIIMMMsgggwwyyCCDDDLIIIMMMsgggwwyyCCDDDLIIIO+IRXMu21efKL+mfw9"
        "g7WjSXXF6CRThNwrDzEl5zVGfZNFBbcFUTGLQiiIUCmW8MMZEi7NldiHi6uYh8urUqpZZJo0sBmdPK+hSSEvWn6+CE1S1jDL4Xl3"
        "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"
        "r5DHvkkOO4sb2R5BJfiUyBms0Gj9TG+gy0XjzFEn4nsEBTkeT354xLmOfe18J93D6MI5pui47/InfjiO7+zIpmsaWbdOo/pW5480"
        "qZlRk5I3xaqu6LYqJX+39XY02CAmMqGIvi4fGU1fvceOUcnNnRk5a491ZJfrPklspeensJqUiW/Q8/T7kKXv4DUQM4byWxY629zA"
        "el16jDrqHIfPVmdzJG+RPWOy1k90S9ajO+G+TrgWPpZc3wnXOsm1QRlqQQYi65AcR5wcdwRkfm1MFz9cHvIb0l8kkzfISio/jul+"
        "DEkx8UdQA1zSz5QcGXy3avC9M09P0kz02/1YEGqdv0FNcElfc/nT4H3H9Pu8aK1FHy9oiZ2A+84M3vfW4H0d+n17PevQopv75BAe"
        "fG/gMf37yQ3vCqR+WX5eEzrZeULn+HnWrrnzTCN6Lsg0pxNf3ZslzPsoyzqPnUb3iey7hnJyTgp5b4/UQSRG6b4zEw3VJ6G/6PU5"
        "+Y0MyA1mqC0sQ3GO1DVnq3PM5KzAUD6g+9PX0nvxpJWjTWdhzAfPF5ncwWc5iMXc2dxGzl30bAp8pnU0wTv4PYkD1C/sW/W9i1CX"
        "Ltug/9hW4Wzb5/0InrHpfvT4Z370/OAeUNcX/67F5f8fzBliyW9QREZvYMjvd5DfnqgvGceRdysXm10EQyaVTzMVmO2Hw1ArgjzN"
        "IGczHXu0vg47QrroHFk+la63FExKY0K87kdf9nsO9Hx0vl0o0H7OhS8kP2yqVMSA0iSqK7YIpB5W3NvMiuewnlcST6JlWfnYjvT/"
        "G+DhjEX44UWL8JeNPSI2cCUvnwxi529frIczD5D3uULMUB1cez1iPtunHn7WRKL71EPz1F5P2Al+wX526Ir76rvTUPPVb6T0Ar4c"
        "P0WH9+/NNrWszeZblmZDr/NGyv6PsvlzxY2MmDYeLTbboddLI2cZTXMBh1CbmM4Im8hvN/Dg31CPIh5625SdOCdlFN7JRkbHmMnB"
        "IMEgbiqJ4sjoTHYLbhT6bx+Fm1ozOXfwn9myp1xMyfkiluw3HYE40VG5k9f7tp1cn2dU5Ihzw0u0tx+YkVJgninUu3PFI86ZWEg7"
        "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"
        "037+G5EJDrk="
    ),
}

NOTICE = """\
==============================================================================
  XDJ-700 PLUS, UNOFFICIAL AND EXPERIMENTAL

  FLASHING FIRMWARE IS AT YOUR OWN RISK.
  It can leave a player unusable. There is no warranty of any kind, and no
  one who made or shared it is responsible for any damage or loss.

  - This is not a Pioneer DJ or AlphaTheta product. They have nothing to do
    with it, do not support it, and may refuse service on a modified player.
  - It is free software under the GNU General Public License version 3,
    provided as is, with no warranty of any kind.
  - You use it entirely at your own risk. In no case is anyone who made or
    shared it responsible for any damage or loss, to your player, your
    media, your gig or anything else.
  - It has been tested at length on the author's XDJ-700s, in sessions of up
    to 18 hours, and has been very stable. That is not proof that it works
    the same on yours. Treat it as experimental, and do not rely on it for a
    show before you have tried it yourself at home.
  - A failed or interrupted update can leave a player unusable, as with any
    firmware update. Never switch the player off during an update.
  - The boot and recovery region and the front panel firmware stay as
    Pioneer made them, so the player's own update can always put the
    official firmware back.
  - Keep the restore file that the patcher writes, in case you want to go
    back to stock.
  - Only for the XDJ-700, and only from your own official XDJ-700 firmware
    version 1.15 UPD file.
"""
NOTICE_END = "=" * 78 + "\n"

APPLICATION_OFFSET = 0x40000           # the packed application inside the MAIN image
MAIN_IMAGE_BYTES = 7251904
APPLICATION_SPAN_BYTES = MAIN_IMAGE_BYTES - APPLICATION_OFFSET
# The updater erases and writes the flash up to here: an image may grow up to it, never into it.
FLASH_LIMIT = 0x7E0000
BOOT_REGION_LINES = 4081               # S-records of the boot region, kept as they are
RECORD_BYTES = 32
WINDOW = 4096
MAX_MATCH = 18
STOCK_TAG = b"MAINVer1.15\x00"
ADDRESS_BYTES = {"0": 2, "1": 2, "2": 3, "3": 4, "5": 2, "6": 3, "7": 4, "8": 3, "9": 2}


class PatchError(Exception):
    """Something is not as it must be; nothing is written."""


# Errors that can only come from our side: each is raised only once the input is the official file.
OURS = ("the packed application is damaged", "the packed application's checksum is wrong",
        "the unpacked application is not the expected one", "the version tag was not found", "internal error:")
REPORT = "this should not happen with the official file: please report it at https://github.com/lowtrustsociety/XDJ-700-PLUS/issues"


def stopped(error, written="nothing was written."):
    """The red line for a refusal, with the report line when it is our fault."""
    text = "\nstopped: %s. %s" % (str(error).rstrip("."), written)
    if str(error).startswith(OURS):
        text += "\n" + REPORT
    return text


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def notice():
    """The notice, with what is good to know about the build this patcher makes."""
    text = NOTICE
    if PATCH_DATA is not None and PATCH_DATA.get("known_issues"):
        text += "\n  good to know:\n"
        for issue in PATCH_DATA["known_issues"]:
            text += textwrap.fill(issue, width=77, initial_indent="  - ", subsequent_indent="    ") + "\n"
    return text + NOTICE_END


def application_span(block_bytes):
    """Bytes of flash the packed application takes: the stock length, or past it to a whole
    record when the mod's code needs more (as the build does)."""
    return max(APPLICATION_SPAN_BYTES, -(-block_bytes // RECORD_BYTES) * RECORD_BYTES)


def split_container(data):
    """The update file: two length lines, then the MAIN and the PANEL component."""
    try:
        first, second, remainder = data.split(b"\r\n", 2)
        lengths = [int(first), int(second)]
    except ValueError:
        raise PatchError("this is not an XDJ-700 update file")
    offset = len(data) - len(remainder)
    if offset + sum(lengths) != len(data):
        raise PatchError("the update file is damaged (lengths do not add up)")
    return lengths, offset


def decode_records(text):
    """The flash image a block of Motorola S-records describes."""
    image = bytearray(b"\xff" * FLASH_LIMIT)
    end = 0
    for line in text.splitlines():
        kind = chr(line[1])
        raw = bytes.fromhex(line[2:].decode("ascii"))
        if line[:1] != b"S" or kind not in ADDRESS_BYTES or raw[0] != len(raw) - 1 or sum(raw) & 0xFF != 0xFF:
            raise PatchError("the update file is damaged (bad S-record)")
        if kind in "123":
            size = ADDRESS_BYTES[kind]
            address = int.from_bytes(raw[1:1 + size], "big")
            payload = raw[1 + size:-1]
            if address + len(payload) > FLASH_LIMIT:
                raise PatchError("the update file is damaged (data outside the image)")
            image[address:address + len(payload)] = payload
            end = max(end, address + len(payload))
    return bytes(image[:end])


def encode_record(address, payload):
    body = address.to_bytes(3, "big") + payload
    raw = bytes([len(body) + 1]) + body
    return b"S2" + (raw + bytes([0xFF - (sum(raw) & 0xFF)])).hex().upper().encode("ascii") + b"\r\n"


def lzss_decode(source):
    """Unpack the firmware's LZSS stream (4,096-byte window, matches of 3 to 18 bytes)."""
    ring = bytearray(b" " * (WINDOW - MAX_MATCH) + b"\0" * MAX_MATCH)
    cursor = WINDOW - MAX_MATCH
    output = bytearray()
    position = 0
    size = len(source)
    while position < size:
        flags = source[position]
        position += 1
        for bit in range(8):
            if position >= size:
                break
            if flags & (1 << bit):
                value = source[position]
                position += 1
                output.append(value)
                ring[cursor] = value
                cursor = (cursor + 1) & (WINDOW - 1)
            else:
                if position + 2 > size:
                    raise PatchError("the packed application is damaged")
                low = source[position]
                high = source[position + 1]
                position += 2
                start = low | ((high & 0xF0) << 4)
                for index in range((high & 0x0F) + 3):
                    value = ring[(start + index) & (WINDOW - 1)]
                    output.append(value)
                    ring[cursor] = value
                    cursor = (cursor + 1) & (WINDOW - 1)
    return bytes(output)


def lzss_encode(data, progress=None):
    """Pack data as the firmware expects it.

    Makes the same choices as the encoder the tested build was made with, so
    the result is the same bytes: the longest match (3 to 18 bytes) starting
    in the last 4,096 positions, the most recent one on a tie, and a single
    byte instead when the next position has a longer match.
    """
    size = len(data)
    rfind = data.rfind
    output = bytearray()
    append = output.append
    position = 0
    next_report = 0

    def find(at, newest):
        limit = min(size - at, MAX_MATCH)
        if limit < 3:
            return 0, 0
        oldest = max(at - WINDOW, 0)
        where = rfind(data[at:at + 3], oldest, newest + 3)
        if where < 0:
            return 0, 0
        best = 3
        low, high = 4, limit
        while low <= high:
            middle = (low + high) >> 1
            found = rfind(data[at:at + middle], oldest, newest + middle)
            if found < 0:
                high = middle - 1
            else:
                best, where = middle, found
                low = middle + 1
        return best, where

    while position < size:
        if progress is not None and position >= next_report:
            progress(position, size)
            next_report = position + size // 50 + 1
        flags_at = len(output)
        append(0)
        flags = 0
        for bit in range(8):
            if position >= size:
                break
            best, where = find(position, position - 1)
            if 3 <= best < MAX_MATCH and size - position > 1 and find(position + 1, position - 1)[0] > best:
                best = 0
            if best >= 3:
                ring = (WINDOW - MAX_MATCH + where) & (WINDOW - 1)
                append(ring & 0xFF)
                append(((ring >> 4) & 0xF0) | (best - 3))
                position += best
            else:
                flags |= 1 << bit
                append(data[position])
                position += 1
        output[flags_at] = flags
    if progress is not None:
        progress(size, size)
    return bytes(output)


def read_official_file(path):
    """The update file's bytes, from the file itself or from the zip Pioneer ships it in."""
    with open(path, "rb") as handle:
        data = handle.read()
    if data[:2] == b"PK":
        try:
            with zipfile.ZipFile(io.BytesIO(data)) as archive:
                names = [name for name in archive.namelist() if name.upper().endswith("XDJ700.UPD")]
                if len(names) != 1:
                    raise PatchError("no XDJ700.UPD in this zip file")
                data = archive.read(names[0])
        except zipfile.BadZipFile:
            raise PatchError("this zip file cannot be read")
    return data


OFFICIAL = "the official XDJ-700 firmware version 1.15 UPD file"
HEADER = re.compile(rb"\r\n([ -~]{1,16}?) *MAINVer([0-9]{1,2})\.([0-9]{1,3})\x00")


def refusal(data):
    """Why a file given as the official one is refused, saying what it is when that can be told; None if it is the official file."""
    digest = sha256(data)
    if digest == PATCH_DATA["stock_sha256"]:
        return None
    if digest == PATCH_DATA["mod_sha256"]:
        return ("this is already the mod (" + PATCH_DATA["build"] + "). nothing to build: copy it as XDJ700.UPD to the "
                "top folder of a FAT32 USB stick. to build it again, with the restore file, provide " + OFFICIAL)
    if digest == PATCH_DATA["restore_sha256"]:
        return ("this is the restore file this patcher makes: it puts the official firmware 1.15 back and cannot be "
                "patched. please provide " + OFFICIAL + ", unchanged")
    header = HEADER.search(data[:160])
    if header:
        player = header.group(1).decode("ascii").strip()
        major, minor = int(header.group(2)), int(header.group(3))
        version = "%d.%s" % (major, header.group(3).decode("ascii"))
        if player != "XDJ-700":
            return "this is the update file of another player (" + player + "). please provide " + OFFICIAL
        if version == PATCH_DATA["mod_tag"]:
            return ("this says it is the mod (version " + version + "), but it is not byte for byte the tested build ("
                    + PATCH_DATA["build"] + "): it is damaged, changed or another build. DO NOT flash it. to build it "
                    "again, provide " + OFFICIAL)
        if (major, minor) > (1, 15):
            return ("this XDJ-700 file says version " + version + ": it has already been modified (a mod or a restore "
                    "file), so it cannot be patched. please provide " + OFFICIAL + ", unchanged")
        if (major, minor) < (1, 15):
            return "this is an older XDJ-700 firmware (version " + version + "). please provide " + OFFICIAL
        return ("this file says XDJ-700 version 1.15, but it is not the official file unchanged: it has been modified "
                "or damaged. please provide " + OFFICIAL + ", unchanged")
    return "this is not " + OFFICIAL + ". please provide that, unchanged. any other file is refused"


def check_official(stock):
    reason = refusal(stock)
    if reason:
        raise PatchError(reason)


def retag(component, tag):
    """The MAIN component with another version tag in its header, and its checksum redone."""
    header = component[:32].replace(STOCK_TAG, b"MAINVer" + tag.encode("ascii") + b"\x00")
    if len(header) != 32 or header == component[:32]:
        raise PatchError("the version tag was not found")
    return header


def assemble_restore(stock):
    """Stock 1.15 under a higher version tag, so the player accepts it over the mod."""
    check_official(stock)
    lengths, offset = split_container(stock)
    main = stock[offset:offset + lengths[0]]
    body = retag(main, PATCH_DATA["restore_tag"]) + main[32:-2]
    return stock[:offset] + body + binascii.crc_hqx(body, 0).to_bytes(2, "little") + stock[offset + lengths[0]:]


def build_restore(stock):
    """The restore file, checked against the one the build made."""
    restore = assemble_restore(stock)
    if sha256(restore) != PATCH_DATA["restore_sha256"]:
        raise PatchError("internal error: the restore file is not the expected one")
    return restore


def assemble_mod(stock, progress=None):
    """The patched update file, made from the official one (build_mod checks it is the tested build)."""
    check_official(stock)
    lengths, offset = split_container(stock)
    main = stock[offset:offset + lengths[0]]
    panel = stock[offset + lengths[0]:]
    lines = main[32:-2].splitlines(keepends=True)
    image = decode_records(main[32:-2])

    packed_bytes = int.from_bytes(image[APPLICATION_OFFSET:APPLICATION_OFFSET + 4], "little")
    packed_end = APPLICATION_OFFSET + 4 + packed_bytes
    if int.from_bytes(image[packed_end:packed_end + 2], "little") != sum(image[APPLICATION_OFFSET:packed_end]) & 0xFFFF:
        raise PatchError("the packed application's checksum is wrong")
    application = lzss_decode(image[APPLICATION_OFFSET + 4:packed_end])
    if sha256(application) != PATCH_DATA["stock_application_sha256"]:
        raise PatchError("the unpacked application is not the expected one")

    # Our bytes at the places the mod hooks in, and our code after the application's end.
    patched = bytearray(application)
    for position, replacement in PATCH_DATA["changes"]:
        new = bytes.fromhex(replacement)
        patched[position:position + len(new)] = new
    patched += zlib.decompress(base64.b64decode(PATCH_DATA["tail"]))
    patched = bytes(patched)
    if sha256(patched) != PATCH_DATA["mod_application_sha256"]:
        raise PatchError("internal error: the patched application is not the expected one")

    packed = lzss_encode(patched, progress)
    if lzss_decode(packed) != patched:
        raise PatchError("internal error: the packed application does not unpack to what went in")
    block = len(packed).to_bytes(4, "little") + packed
    block += (sum(block) & 0xFFFF).to_bytes(2, "little")
    span = application_span(len(block))
    if APPLICATION_OFFSET + span > FLASH_LIMIT:
        raise PatchError("internal error: the packed application does not fit")
    block += b"\xff" * (span - len(block))

    records = [
        encode_record(APPLICATION_OFFSET + index, block[index:index + RECORD_BYTES])
        for index in range(0, span, RECORD_BYTES)
    ]
    body = retag(main, PATCH_DATA["mod_tag"]) + b"".join(lines[:BOOT_REGION_LINES]) + b"".join(records) + lines[-1]
    new_main = body + binascii.crc_hqx(body, 0).to_bytes(2, "little")
    mod = str(len(new_main)).encode("ascii") + b"\r\n" + str(len(panel)).encode("ascii") + b"\r\n" + new_main + panel

    # The two rules every build keeps: the boot region and the panel part are Pioneer's, untouched.
    if decode_records(new_main[32:-2])[:APPLICATION_OFFSET] != image[:APPLICATION_OFFSET]:
        raise PatchError("internal error: the boot region changed")
    if mod[len(mod) - len(panel):] != panel:
        raise PatchError("internal error: the panel part changed")
    return mod


def build_mod(stock, progress=None):
    """The patched update file, byte for byte the build that was tested on the player."""
    mod = assemble_mod(stock, progress)
    if sha256(mod) != PATCH_DATA["mod_sha256"]:
        raise PatchError("internal error: the result is not the tested build")
    return mod


def show_progress(done, total):
    width = 40
    filled = width * done // total if total else width
    sys.stdout.write("\r  packing [" + "#" * filled + "." * (width - filled) + "] %3d%%" % (100 * done // total))
    if done >= total:
        sys.stdout.write("\n")
    sys.stdout.flush()


def write_file(folder, data):
    os.makedirs(folder, exist_ok=True)
    path = os.path.join(folder, "XDJ700.UPD")
    with open(path, "wb") as handle:
        handle.write(data)
    with open(path, "rb") as handle:
        if handle.read() != data:
            raise PatchError("the file written to " + path + " does not read back the same")
    return path


# Green when good, red when bad, as on the page; plain where the output is not a terminal.
COLOURS = {"ok": "\033[32m", "bad": "\033[31m"}


def say(text, kind=None):
    if kind and sys.stdout.isatty():
        if os.name == "nt":
            os.system("")  # lets the Windows console show colours
        text = COLOURS[kind] + text + "\033[0m"
    print(text)


def identify(data):
    """What a file is: "mod", "restore", "official" (Pioneer's 1.15), "damaged" (says it is the mod but is not),
    "zip" or None."""
    if data[:2] == b"PK":
        return "zip"
    digest = sha256(data)
    if digest == PATCH_DATA["mod_sha256"]:
        return "mod"
    if digest == PATCH_DATA["restore_sha256"]:
        return "restore"
    if digest == PATCH_DATA["stock_sha256"]:
        return "official"
    header = HEADER.search(data[:160])
    if header and header.group(1).strip() == b"XDJ-700" and \
            (header.group(2) + b"." + header.group(3)).decode("ascii") == PATCH_DATA["mod_tag"]:
        return "damaged"
    return None


def zip_inside(data):
    """What the XDJ700.UPD files in a zip are, as a set of identify() answers."""
    try:
        with zipfile.ZipFile(io.BytesIO(data)) as archive:
            return {identify(archive.read(name)) for name in archive.namelist() if name.upper().endswith("XDJ700.UPD")}
    except (zipfile.BadZipFile, OSError, RuntimeError, ValueError):
        return set()


AGAIN = ("copy XDJ700.UPD from the zip or folder made here to the top folder of your USB stick again, eject the stick, plug it "
         "back in and try again.")


def stick_check(data, name):
    """The check of the file on the stick: (the answer, True if it is safe to flash). The page gives the same answers."""
    kind = identify(data)
    wrong_name = "" if name.upper() == "XDJ700.UPD" else (
        " but it is called " + name + " on the stick: rename it to XDJ700.UPD, or the player will not see it.")
    if kind == "mod":
        return "confirmed: this is the tested build (" + PATCH_DATA["build"] + "). safe to flash." + wrong_name, not wrong_name
    if kind == "restore":
        return ("confirmed: this is the restore file (the official firmware 1.15). safe to flash to go back."
                + wrong_name, not wrong_name)
    if kind == "official":
        return ("this is the official firmware 1.15 as Pioneer ships it, not a file made here. to install the mod, "
                "copy 1_mod/XDJ700.UPD from the zip or folder made here; to go back to stock, copy 2_stock_restore/XDJ700.UPD.", False)
    if kind == "damaged":
        return ("this says it is the mod (version " + PATCH_DATA["mod_tag"] + "), but it is not byte for byte the "
                "tested build (" + PATCH_DATA["build"] + "): it is damaged, changed or another build. DO NOT flash it. "
                + AGAIN, False)
    if kind == "zip":
        inside = zip_inside(data)
        if "mod" in inside:
            return ("this is the mod, still in its zip. the player cannot flash from a zip: open it, copy the mod's "
                    "XDJ700.UPD to the top folder of your USB stick, then drop that file here.", False)
        if "restore" in inside:
            return ("this is the restore file, still in its zip. the player cannot flash from a zip: open it, copy the "
                    "XDJ700.UPD inside to the top folder of your USB stick, then drop that file here.", False)
        if "official" in inside:
            return ("this is Pioneer's zip with the official firmware 1.15, not a file made here. to install the mod, "
                    "copy 1_mod/XDJ700.UPD from the zip or folder made here.", False)
    return "this file is not right. DO NOT flash it. " + AGAIN, False


def check_file(path):
    """The check of the file on the stick, with the page's answers. True if it is safe to flash."""
    path = path.strip()
    if len(path) > 1 and path[0] == path[-1] and path[0] in "'\"":
        path = path[1:-1]
    elif os.name != "nt":
        path = path.replace("\\ ", " ")  # a file dropped into a terminal comes with its spaces escaped
    name = os.path.basename(path)
    print("checking " + name + "...")
    try:
        with open(path, "rb") as handle:
            data = handle.read()
    except OSError:
        say("the file could not be read", "bad")
        return False
    answer, ok = stick_check(data, name)
    say(answer, "ok" if ok else "bad")
    return ok


STEPS = """
what to do now:

  1. the two files:
       the mod:            %s
       the restore file:   %s
  2. copy the mod to the stick. copy the mod's XDJ700.UPD to the top folder
     of a FAT32 USB stick with about 18 MB free. keep the 2_stock_restore
     folder on your computer: it holds the restore file, which puts the
     official firmware 1.15 back.
  3. eject the stick properly, then plug it back in. this matters for the
     next step: without it the computer may answer from its own memory
     instead of reading the stick.
  4. check the file on the stick: drop XDJ700.UPD from the stick into this
     window and press enter. flash only after it is confirmed.
     to check again later:
       python3 %s --check <the XDJ700.UPD on the stick>
  5. flash. with the player switched off, put the stick in. hold IN and
     RELOOP/EXIT, switch the player on, and keep holding until the update
     screen appears. wait until the update has finished and the player has
     restarted. DO NOT SWITCH IT OFF before that.

  *  going back to stock: the same steps with the XDJ700.UPD from the
  2_stock_restore folder. the update screen shows %s for the mod and
  %s for the restore file: those numbers are only there so that the
  player accepts the file.
"""


def ask_checks():
    """Step 4 in the terminal: files dropped into the window are checked until one is right."""
    while True:
        try:
            answer = input("\ndrop the XDJ700.UPD from your USB stick here and press enter (enter alone to stop): ")
        except EOFError:
            return
        if not answer.strip():
            return
        if check_file(answer):
            return


def main(arguments=None):
    parser = argparse.ArgumentParser(
        description="patch your own official XDJ-700 firmware version 1.15 UPD file with the XDJ-700 PLUS mod.",
        epilog="unofficial and experimental. flashing firmware is at your own risk.",
    )
    parser.add_argument("official", nargs="?", help="your own official XDJ-700 firmware version 1.15 UPD file")
    parser.add_argument("--output-dir", default="xdj700_plus", help="where to write the two files (default: %(default)s)")
    parser.add_argument("--yes", action="store_true", help="i have read the notice and accept the risk; do not ask")
    parser.add_argument("--check", metavar="FILE", help="only check a file on the USB stick: is it safe to flash?")
    options = parser.parse_args(arguments)

    print("XDJ-700 PLUS patcher")
    if PATCH_DATA is None:
        print("this is the template, without the mod's code. use the generated xdj700_plus_patcher.py.")
        return 2
    print(PATCH_DATA["build"] + (" (" + PATCH_DATA["date"] + ")" if PATCH_DATA.get("date") else ""))
    if options.check:
        return 0 if check_file(options.check) else 1
    if options.official is None:
        parser.print_usage()
        print("\nyou have to provide your own official XDJ-700 firmware version 1.15 UPD file.")
        print("you will also need a FAT32 USB stick with about 18 MB free.")
        return 2
    print("you will also need a FAT32 USB stick with about 18 MB free.")

    try:
        print("\nreading " + os.path.basename(options.official) + "...")
        stock = read_official_file(options.official)
        if identify(stock) == "mod":
            say(refusal(stock) + ".", "ok")
            print("to check it on the stick: python3 " + (os.path.basename(sys.argv[0]) or "xdj700_plus_patcher.py")
                  + " --check <the XDJ700.UPD on the stick>")
            return 0
        check_official(stock)
        say("official XDJ-700 firmware version 1.15 UPD file detected.", "ok")
        print("\n" + notice())
        if not options.yes:
            if not sys.stdin.isatty():
                raise PatchError("not asked and not confirmed: run it in a terminal, or add --yes after reading the notice")
            print("i have read this and accept the risk.")
            answer = input("type yes to accept and build the mod: ")
            if answer.strip().lower() != "yes":
                print("nothing was written.")
                return 1
        print("\nbuilding the mod (about a minute)...")
        mod = build_mod(stock, show_progress)
        print("checking the result...")
        restore = build_restore(stock)
        mod_path = write_file(os.path.join(options.output_dir, "1_mod"), mod)
    except (PatchError, OSError) as error:
        say(stopped(error), "bad")
        return 1
    try:
        restore_path = write_file(os.path.join(options.output_dir, "2_stock_restore"), restore)
    except (PatchError, OSError) as error:
        say(stopped(error, "the mod was written to " + mod_path + ", the restore file was not: run it again."), "bad")
        return 1

    say("\ndone. both files were checked: the mod is byte for byte the tested build.", "ok")
    print(STEPS % (mod_path, restore_path, os.path.basename(sys.argv[0]) or "xdj700_plus_patcher.py",
                   PATCH_DATA["mod_tag"], PATCH_DATA["restore_tag"]))
    if sys.stdin.isatty() and not options.yes:
        ask_checks()
    return 0


if __name__ == "__main__":
    sys.exit(main())
