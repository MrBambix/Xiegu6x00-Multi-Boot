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
    $cc -O2 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -I"$RI" -I"$TA/include" -I"$W" "$@" \
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

# A test cartridge of our own (NROM, 16 KB + 8 KB): a light blue screen and a 440 Hz tone
python3 - "$W/test.nes" <<'PY'
import sys
code = bytes([
    0x78, 0xD8,                    # SEI, CLD
    0xA2, 0x40, 0x8E, 0x17, 0x40,  # LDX #$40, STX $4017
    0xA2, 0xFF, 0x9A, 0xE8,        # LDX #$FF, TXS, INX
    0x8E, 0x00, 0x20, 0x8E, 0x01, 0x20, 0x8E, 0x10, 0x40,   # $2000 $2001 $4010 = 0
    0x2C, 0x02, 0x20, 0x10, 0xFB,  # vblank 1
    0x2C, 0x02, 0x20, 0x10, 0xFB,  # vblank 2
    0xA9, 0x3F, 0x8D, 0x06, 0x20, 0xA9, 0x00, 0x8D, 0x06, 0x20,   # PPU address $3F00
    0xA9, 0x21, 0x8D, 0x07, 0x20,  # the backdrop: light blue
    0xA9, 0x00, 0x8D, 0x06, 0x20, 0x8D, 0x06, 0x20,              # address back to 0
    0xA9, 0x0A, 0x8D, 0x01, 0x20,  # background on
    0xA9, 0x01, 0x8D, 0x15, 0x40,  # pulse 1 on
    0xA9, 0xBF, 0x8D, 0x00, 0x40,  # duty 50 %, constant volume 15
    0xA9, 0xFD, 0x8D, 0x02, 0x40, 0xA9, 0x00, 0x8D, 0x03, 0x40,   # timer 253: 440 Hz
    0x4C, 0x47, 0xC0,              # loop: JMP to itself
    0x40,                          # NMI / IRQ: RTI
])
loop = code.index(bytes([0x4C, 0x47, 0xC0]))
code = code[:loop] + bytes([0x4C, loop & 0xFF, 0xC0 | (loop >> 8)]) + code[loop + 3:]
rti = 0xC000 + len(code) - 1
prg = bytearray(code.ljust(0x4000, b"\xff"))
prg[0x3FFA:0x4000] = bytes([rti & 0xFF, rti >> 8, 0x00, 0xC0, rti & 0xFF, rti >> 8])
open(sys.argv[1], "wb").write(b"NES\x1a\x01\x01\x00\x00" + bytes(8) + bytes(prg) + bytes(0x2000))
PY

# A PC test: the test cartridge for 120 frames, drawn into memory, then the menu over it
if command -v gcc >/dev/null; then
    build gcc "$W/pc"
    gcc -o "$W/nes-pc" "$W"/pc/*.o $(find "$W/pc/fceumm/src" -name '*.o') -lm -lpthread
    (cd "$W" && NES_TEST_FB=480x800 NES_TEST_FRAMES=120 NES_TEST_OUT=frame.raw NES_TEST_MENU_OUT=menu.raw NES_TEST_BT_OUT=bt.raw NES_TEST_PCM=sound.raw \
        NES_TEST_ROM="$W/test.nes" timeout 120 ./nes-pc > pc-test.log 2>&1)
    test "$(stat -c %s "$W/frame.raw")" = 1536000
    python3 - "$W/frame.raw" "$W/sound.raw" <<'PY'
import struct, sys
fb = open(sys.argv[1], "rb").read()
# the screen is portrait 480x800, the game landscape: count the game's light blue (R<G<B, B high)
px = struct.unpack("<%dI" % (len(fb) // 4), fb)
blue = sum(1 for p in px if (p & 0xff) > 0xc0 and ((p >> 16) & 0xff) < 0xa0)
print("PC test picture: %d light blue pixels" % blue)
assert blue > 50000, "the game's picture did not come out"
d = open(sys.argv[2], "rb").read()
n = len(d) // 2
s = struct.unpack("<%dh" % n, d[:n * 2])
loud = sum(1 for v in s if abs(v) > 500)
print("PC test sound: %.2f s, %d%% of samples above -36 dBFS" % (n / 2 / 48000, 100 * loud // max(n, 1)))
assert n > 48000 and loud > n // 4, "no tone came out"
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
        NES_TEST_KEYS="$(printf 'w%.0s' $(seq 1 85))" timeout 120 "$W/nes-pc") &
    sleep 1
    python3 "$HERE/test/drive_input.py" "$T/in" > "$T/drive.log"
    wait
    for bits in 00180 00040 00100 00008 10000 00001; do grep -q "test bits p1=$bits" "$T/in.log"; done
    grep -q "UP=A17- DOWN=A17+ LEFT=A0- RIGHT=A0+ A=K308" "$T/roms/pads.txt"
    grep -q "B=K307 TB=K318 SELECT=K310 START=K311" "$T/roms/pads.txt"
    echo "PC test input: pad, keyboard, the menu combination, learnt buttons: ok"
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
