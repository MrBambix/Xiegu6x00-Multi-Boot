#!/bin/bash
# Builds the NES emulator for the X6100: the FCEUmm libretro core with our front end
# (nes_x6100.c), static, as /sbin/init of a small ext4 root filesystem. No game comes with it: the
# user's own go in the NES folder of the card's BOOT partition. The kernel comes from a system
# already on the card, as Doom's. Result: extras/nes/out/nes-x6100-rootfs.ext4.xz
#
# Needs: gcc-arm-linux-gnueabihf, e2fsprogs (mke2fs -d), git, make, python3, xz; gcc for the PC test.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
W=$HERE/work
OUT=$HERE/out
FC_COMMIT=ac0d00a7fc01166f84c7bac01d1084ebfd2b865e   # libretro-fceumm (GPL-2.0), 2026-10-11
TA_COMMIT=1c5fb68ced57d838f2b7ecd0c00bc1fefc9ab60d   # tinyalsa v2.0.0 (BSD-3-Clause)
rm -rf "$W" && mkdir -p "$W" "$OUT"

git clone -q https://github.com/libretro/libretro-fceumm "$W/fceumm"
git -C "$W/fceumm" checkout -q $FC_COMMIT
git clone -q https://github.com/tinyalsa/tinyalsa "$W/tinyalsa"
git -C "$W/tinyalsa" checkout -q $TA_COMMIT
TA=$W/tinyalsa
RI=$W/fceumm/src/drivers/libretro/libretro-common/include
# The menus' font: Inter (SIL OFL), rendered smooth into a C header (the fonts-inter package's)
INTER=$(dirname "$(find /usr/share/fonts -name Inter-Medium.otf | head -1)")
python3 "$HERE/make_font.py" "$INTER" "$W/font_inter.h"

build() {   # build CC OUTDIR EXTRA_FLAGS...
    local cc=$1 out=$2; shift 2
    mkdir -p "$out"
    # the core, by its own makefile (its objects are taken, not its .so)
    rm -rf "$out/fceumm" && cp -r "$W/fceumm" "$out/fceumm"
    make -s -C "$out/fceumm" -f Makefile.libretro -j"$(nproc)" platform=unix HAVE_NTSC=0 \
        CC="$cc $*" >"$out/core.log" 2>&1 || { tail -30 "$out/core.log"; exit 1; }
    for f in pcm pcm_hw mixer mixer_hw limits snd_card_plugin; do
        $cc -O2 -w -I"$TA/include" "$@" -c "$TA/src/$f.c" -o "$out/ta_$f.o"
    done
    $cc -O2 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -I"$RI" -I"$TA/include" -I"$W" -I/usr/include/stb "$@" \
        -c "$HERE/nes_x6100.c" -o "$out/nes_x6100.o"
    # A name the front end takes from the C library must not be one the core defines too: the
    # static link takes the core's (FCEUmm's global "sync" variable stood in for sync() so)
    local clash
    clash=$(comm -12 <(nm -u "$out/nes_x6100.o" | awk '{print $2}' | grep -v '^retro_' | sort -u) \
        <(nm -g --defined-only $(find "$out/fceumm/src" -name '*.o') | awk 'NF==3{print $3}' | sort -u))
    if [ -n "$clash" ]; then echo "names the core defines too: $clash"; exit 1; fi
}

# The radio's: ARMv7, hard float, one static binary
build arm-linux-gnueabihf-gcc "$W/arm" -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard
arm-linux-gnueabihf-gcc -static -s -o "$W/nes" "$W"/arm/*.o $(find "$W/arm/fceumm/src" -name '*.o') -lm -lpthread

# A test cartridge of our own (NROM, 16 KB + 8 KB), alive to see and hear: every second the
# screen turns blue with a 440 Hz tone, then green and quiet (a frozen game shows at once)
python3 - "$W/test.nes" <<'PY'
import sys
code, labels, fix = bytearray(), {}, []
def op(*b): code.extend(b)
def label(n): labels[n] = 0xC000 + len(code)
def jmp_abs(opc, n): code.append(opc); fix.append(("abs", len(code), n)); code.extend(b"\0\0")
def branch(opc, n): code.append(opc); fix.append(("rel", len(code), n)); code.append(0)
def sta(a): op(0x8D, a & 0xFF, a >> 8)
def lda(v): op(0xA9, v)
label("reset")
op(0x78, 0xD8, 0xA2, 0x40, 0x8E, 0x17, 0x40, 0xA2, 0xFF, 0x9A, 0xE8)      # SEI CLD, frame IRQ off, stack
op(0x8E, 0x00, 0x20, 0x8E, 0x01, 0x20, 0x8E, 0x10, 0x40, 0x86, 0x00, 0x86, 0x01)
for v in ("vb1", "vb2"):                                                   # two vblanks: the PPU ready
    label(v); op(0x2C, 0x02, 0x20); branch(0x10, v)
lda(0x3F); sta(0x2006); lda(0x00); sta(0x2006); lda(0x21); sta(0x2007)  # the backdrop: blue
lda(0x00); sta(0x2006); sta(0x2006)
lda(0x0A); sta(0x2001)                                                     # background on
lda(0x01); sta(0x4015); lda(0xBF); sta(0x4000); lda(0xFD); sta(0x4002); lda(0x00); sta(0x4003)  # 440 Hz
lda(0x80); sta(0x2000)                                                     # NMI on
label("loop"); jmp_abs(0x4C, "loop")
label("nmi")
op(0x48, 0xE6, 0x00, 0xA5, 0x00, 0xC9, 60); branch(0xD0, "done")          # every 60 frames:
op(0xA9, 0x00, 0x85, 0x00, 0xA5, 0x01, 0x49, 0x01, 0x85, 0x01)            # the phase turns
lda(0x3F); sta(0x2006); lda(0x00); sta(0x2006)
op(0xA5, 0x01); branch(0xF0, "blue"); lda(0x2A); branch(0xD0, "set")       # green or blue
label("blue"); lda(0x21)
label("set"); sta(0x2007)
lda(0x00); sta(0x2006); sta(0x2006); sta(0x2005); sta(0x2005)
op(0xA5, 0x01, 0x49, 0x01); sta(0x4015); branch(0xF0, "done")             # the tone on with blue
lda(0x00); sta(0x4003)
label("done"); op(0x68, 0x40)                                              # PLA RTI
label("irq"); op(0x40)
for kind, at, n in fix:
    t = labels[n]
    if kind == "abs":
        code[at], code[at + 1] = t & 0xFF, t >> 8
    else:
        d = t - (0xC000 + at + 1)
        assert -128 <= d < 128
        code[at] = d & 0xFF
prg = bytearray(bytes(code).ljust(0x4000, b"\xff"))
for i, n in enumerate(("nmi", "reset", "irq")):
    prg[0x3FFA + 2 * i], prg[0x3FFB + 2 * i] = labels[n] & 0xFF, labels[n] >> 8
open(sys.argv[1], "wb").write(b"NES\x1a\x01\x01\x00\x00" + bytes(8) + bytes(prg) + bytes(0x2000))
PY

# A PC test: the test cartridge for 120 frames, drawn into memory, then the menu over it
if command -v gcc >/dev/null; then
    build gcc "$W/pc"
    gcc -o "$W/nes-pc" "$W"/pc/*.o $(find "$W/pc/fceumm/src" -name '*.o') -lm -lpthread
    (cd "$W" && NES_TEST_FB=480x800 NES_TEST_FRAMES=150 NES_TEST_OUT=frame.raw NES_TEST_MENU_OUT=menu.raw NES_TEST_BT_OUT=bt.raw NES_TEST_PCM=sound.raw \
        NES_TEST_ROM="$W/test.nes" timeout 120 ./nes-pc > pc-test.log 2>&1)
    test "$(stat -c %s "$W/frame.raw")" = 1536000
    python3 - "$W/frame.raw" "$W/sound.raw" <<'PY'
import struct, sys
fb = open(sys.argv[1], "rb").read()
# the screen is portrait 480x800, the game landscape: count the game's light blue (R<G<B, B high)
px = struct.unpack("<%dI" % (len(fb) // 4), fb)
blue = sum(1 for p in px if ((p & 0xff) > 0xc0 or ((p >> 8) & 0xff) > 0xc0) and ((p >> 16) & 0xff) < 0xa0)
print("PC test picture: %d light blue or green pixels" % blue)
assert blue > 50000, "the game's picture did not come out"
d = open(sys.argv[2], "rb").read()
n = len(d) // 2
s = struct.unpack("<%dh" % n, d[:n * 2])
loud = sum(1 for v in s if abs(v) > 500)
print("PC test sound: %.2f s, %d%% of samples above -36 dBFS" % (n / 2 / 48000, 100 * loud // max(n, 1)))
assert n > 48000 and loud > n // 6, "no tone came out"
peak = max(abs(v) for v in s)
print("PC test sound: peak %d (of 32767)" % peak)
assert peak > 12000, "too quiet"
PY
    # Bluetooth, with a pretend bluetoothctl (BlueZ 5.65's words): a pad asking to confirm, a
    # keyboard showing a code, both paired; the paired list, a device connected. The keys: the
    # list -> Bluetooth -> look, pair the pad -> OK -> look, pair the keyboard -> OK -> paired ->
    # the first -> connect -> back out
    T=$W/pctest && mkdir -p "$T/roms" "$T/in" && cp "$W/test.nes" "$T/roms/"
    (cd "$T" && NES_TEST_LOG="$T/bt.log" NES_TEST_FB=480x800 NES_TEST_ROMS="$T/roms" \
        NES_TEST_BTCTL="$HERE/test/fake_bluetoothctl.py" \
        NES_TEST_KEYS="wwwdowowwwwwwwwwowwwwwwwowwowwwwwwwwwowwwwwwwwwwwwowwdowwwowowwwbbbbb" \
        timeout 120 "$W/nes-pc")
    grep -q "Confirm passkey 123456 (yes/no):  (answered)" "$T/bt.log"
    test "$(grep -c 'bt: pairing BB:00:00:00:00:0[12]: ok' "$T/bt.log")" = 2
    grep -q "bt> connect AA:00:00:00:00:01" "$T/bt.log"
    echo "PC test Bluetooth: pairing with a confirmation and with a keyboard code, connecting: ok"
    # Pads, keyboard, mouse from FIFOs: the bits each gives, Select+Start as the menu, a pad's
    # buttons learnt (an analog trigger at rest taken for none)
    for f in pad kbd mouse; do mkfifo "$T/in/$f"; done
    (cd "$T" && NES_TEST_LOG="$T/in.log" NES_TEST_FB=480x800 NES_TEST_ROMS="$T/roms" \
        NES_TEST_DEVS="pad:$T/in/pad,kbd:$T/in/kbd,mouse:$T/in/mouse" \
        NES_TEST_KEYS="$(printf 'w%.0s' $(seq 1 140))" timeout 150 "$W/nes-pc") &
    sleep 1
    python3 "$HERE/test/drive_input.py" "$T/in" > "$T/drive.log"
    wait
    for bits in 00180 00040 00100 00008 10000 00001; do grep -q "test bits p1=$bits" "$T/in.log"; done
    grep -q "test bits p1=00080" "$T/in.log"    # the hat after learning: the d-pad kept
    grep -q "UP=A17- DOWN=A17+ LEFT=A0- RIGHT=A0+ A=K308" "$T/roms/pads.txt"
    grep -q "B=K307 TB=K318 SELECT=K310 START=K311" "$T/roms/pads.txt"
    echo "PC test input: pad, keyboard, the menu combination, learnt buttons: ok"
    # The game's pace with the sound thread and a pretend card 1 % fast (as the radio's): the
    # game at 60 frames/s, the ring kept, no gaps once it plays
    rm -rf "$T/pace" && mkdir -p "$T/pace" && cp "$W/test.nes" "$T/pace/"   # no saves: no question first
    (cd "$T" && NES_TEST_SNDTHREAD=1 NES_TEST_LOG="$T/pace.log" NES_TEST_FB=480x800 NES_TEST_ROMS="$T/pace" \
        NES_TEST_KEYS="wwwowo$(printf 'w%.0s' $(seq 1 170))" timeout 200 "$W/nes-pc" >/dev/null 2>&1)
    python3 - "$T/pace.log" <<'PY'
import re, sys
st = [l for l in open(sys.argv[1]) if "the card took" in l]
assert len(st) >= 4, "too few sound lines"
fps = [float(re.search(r"\] ([\d.]+) frames/s", l).group(1)) for l in st[1:]]
short = [int(re.search(r"(\d+) short", l).group(1)) for l in st[1:]]
under = [int(re.search(r"(\d+) underruns", l).group(1)) for l in st[1:]]
print("PC test pace: %s frames/s, gaps %s, underruns %s" % (fps, short, under))
assert all(59.5 < f < 60.7 for f in fps), "the game is not at 60 frames/s"
assert short[-1] == short[0] and under[-1] == 0, "the sound ran dry"
wait = [int(re.search(r"(\d+) waiting", l).group(1)) for l in st[1:]]
print("PC test pace: the ring %s (its mark 1536)" % wait)
assert min(wait[1:]) > 900, "the ring runs low: the card's pace not caught"
PY
    cp "$W/frame.raw" "$OUT/test-frame-480x800-bgrx.raw"
    cp "$W/menu.raw" "$OUT/test-menu-480x800-bgrx.raw"
    cp "$W/bt.raw" "$OUT/test-bt-480x800-bgrx.raw"
fi

# The root filesystem
R=$W/root
mkdir -p "$R/sbin" "$R/nes/bt" "$R/dev" "$R/proc" "$R/sys" "$R/tmp" "$R/boot"
cp "$W/nes" "$R/sbin/init"
cp "$W/fceumm/Copying" "$R/nes/FCEUMM-COPYING.txt"
cp "$TA/NOTICE" "$R/nes/TINYALSA-NOTICE.txt"
cp "$(find /usr/share/doc/fonts-inter -name copyright | head -1)" "$R/nes/INTER-FONT-LICENSE.txt" 2>/dev/null || true
cat > "$R/nes/README.txt" <<'TXT'
A NES emulator on the Xiegu X6100 (an extra of Xiegu Multi Patcher, by SQ6RT).
Core: FCEUmm (libretro-fceumm, GPL-2.0) with an X6100 front end (GPL-2.0-or-later); sound through
tinyalsa (BSD-3-Clause); the menus' font: Inter by Rasmus Andersson (SIL Open Font License 1.1).
No game comes with it: your own .nes / .fds / .unf / .nsf files go in the NES folder of the card's
BOOT partition. Sources: https://github.com/TomekR1/Xiegu6x00-Multi-Boot (extras/nes) and
https://github.com/libretro/libretro-fceumm
TXT
rm -f "$OUT/nes-x6100-rootfs.ext4"
mke2fs -q -t ext4 -L NES -O ^metadata_csum_seed -d "$R" "$OUT/nes-x6100-rootfs.ext4" 16M
e2fsck -n -f "$OUT/nes-x6100-rootfs.ext4"
xz -T0 -9 -f "$OUT/nes-x6100-rootfs.ext4"
sha256sum "$OUT/nes-x6100-rootfs.ext4.xz" > "$OUT/nes-x6100-rootfs.ext4.xz.sha256"
ls -la "$OUT"
