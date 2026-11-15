"""Writes input events into the emulator's test FIFOs at set times (struct input_event, x86-64)."""
import os, struct, sys, time

d = sys.argv[1]
EV_SYN, EV_KEY, EV_REL, EV_ABS = 0, 1, 2, 3
BTN = dict(SOUTH=0x130, EAST=0x131, C=0x132, NORTH=0x133, WEST=0x134, TL=0x136, TR=0x137, TL2=0x138, TR2=0x139,
           SELECT=0x13a, START=0x13b, MODE=0x13c, THUMBL=0x13d, THUMBR=0x13e, LEFT=0x110)
KEY = dict(ESC=1, ENTER=28, X=45, Z=44, UP=103, DOWN=108)
ABS = dict(X=0, Y=1, Z=2, HAT0X=0x10, HAT0Y=0x11)
fifos = {}
for n in ("pad", "kbd", "mouse"):
    fifos[n] = os.open(os.path.join(d, n), os.O_WRONLY)


def ev(dev, t, c, v):
    now = time.time()
    os.write(fifos[dev], struct.pack("llHHi", int(now), int((now % 1) * 1e6), t, c, v) +
             struct.pack("llHHi", int(now), 0, EV_SYN, 0, 0))


def tap(dev, t, c, hold=0.15):
    ev(dev, t, c, 1); time.sleep(hold); ev(dev, t, c, 0); time.sleep(0.35)


def axis(code, v, back, hold=0.2):
    ev("pad", EV_ABS, code, v); time.sleep(hold); ev("pad", EV_ABS, code, back); time.sleep(0.4)


def log(m):
    print("%.1f %s" % (time.time() - t0, m), flush=True)


t0 = time.time()
time.sleep(2.0)
log("list: hat down, up, A starts the game")
axis(ABS["HAT0Y"], 1, 0); axis(ABS["HAT0Y"], -1, 0); tap("pad", EV_KEY, BTN["EAST"])
time.sleep(1.0)
log("game: hat right + A held")
ev("pad", EV_ABS, ABS["HAT0X"], 1); ev("pad", EV_KEY, BTN["EAST"], 1); time.sleep(0.5)
ev("pad", EV_ABS, ABS["HAT0X"], 0); ev("pad", EV_KEY, BTN["EAST"], 0); time.sleep(0.5)
log("game: stick left (ABS_X 0), C (=A)")
axis(ABS["X"], 0, 128, 0.4); tap("pad", EV_KEY, BTN["C"], 0.4)
log("game: trigger ABS_Z moved (bound to nothing)")
axis(ABS["Z"], 255, 0, 0.3)
log("game: keyboard X, Enter; mouse right + left button")
tap("kbd", EV_KEY, KEY["X"], 0.4); tap("kbd", EV_KEY, KEY["ENTER"], 0.4)
ev("mouse", EV_REL, 0, 25); ev("mouse", EV_KEY, BTN["LEFT"], 1); time.sleep(0.3); ev("mouse", EV_KEY, BTN["LEFT"], 0); time.sleep(0.5)
log("game: Share+Options (TL2+TR2) = menu; B = back to the game")
ev("pad", EV_KEY, BTN["TL2"], 1); ev("pad", EV_KEY, BTN["TR2"], 1); time.sleep(0.3)
ev("pad", EV_KEY, BTN["TL2"], 0); ev("pad", EV_KEY, BTN["TR2"], 0); time.sleep(1.0)
tap("pad", EV_KEY, BTN["SOUTH"]); time.sleep(1.0)
log("game: Esc = menu, down x5 to Pad buttons, A")
tap("kbd", EV_KEY, KEY["ESC"]); time.sleep(0.8)
for _ in range(5):
    axis(ABS["HAT0Y"], 1, 0, 0.1)
tap("pad", EV_KEY, BTN["EAST"]); time.sleep(1.0)
log("learning: trigger noise first, then up down left right A B select start turboA turboB")
ev("pad", EV_ABS, ABS["Z"], 0); time.sleep(0.6)
axis(ABS["HAT0Y"], -1, 0); time.sleep(0.4)
axis(ABS["HAT0Y"], 1, 0); time.sleep(0.4)
axis(ABS["X"], 0, 128); time.sleep(0.4)
axis(ABS["X"], 255, 128); time.sleep(0.4)
for b in ("WEST", "NORTH", "TL", "TR", "THUMBL", "THUMBR"):
    tap("pad", EV_KEY, BTN[b]); time.sleep(0.4)
time.sleep(3.5)
log("after learning: WEST should be A, NORTH B")
ev("pad", EV_KEY, BTN["WEST"], 1); time.sleep(0.4); ev("pad", EV_KEY, BTN["WEST"], 0); time.sleep(0.4)
ev("pad", EV_KEY, BTN["NORTH"], 1); time.sleep(0.4); ev("pad", EV_KEY, BTN["NORTH"], 0); time.sleep(0.4)
log("guide button = menu")
tap("pad", EV_KEY, BTN["MODE"]); time.sleep(1.0)
tap("pad", EV_KEY, BTN["NORTH"]); time.sleep(1)
log("done")
