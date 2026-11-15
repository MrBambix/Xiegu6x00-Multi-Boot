#!/usr/bin/env python3
"""A pretend bluetoothctl (BlueZ 5.65's words and manners) for the emulator's PC test."""
import os, sys, threading, time

P = "\x1b[0;94m[bluetooth]\x1b[0m# "
lock = threading.Lock()
devs = {
    "AA:00:00:00:00:01": dict(name="8BitDo SN30 Pro", icon="input-gaming", paired=True, conn=False, trusted=True),
    "AA:00:00:00:00:02": dict(name="Logitech K380", icon="input-keyboard", paired=True, conn=True, trusted=True),
}
found = [
    ("BB:00:00:00:00:01", dict(name="Xbox Wireless Controller", icon="input-gaming")),
    ("BB:00:00:00:00:02", dict(name="BT Keyboard", icon="input-keyboard")),
    ("BB:00:00:00:00:03", dict(name="", icon="")),
    ("BB:00:00:00:00:04", dict(name="Telefon Ani", icon="phone")),
]
waiting = {}
out = sys.stdout


def say(line, prompt=True):
    with lock:
        out.write("\r\x1b[K" + line + "\n" + (P if prompt else ""))
        out.flush()


def ask(q):
    with lock:
        out.write("\r\x1b[K" + q)
        out.flush()


def later(sec, fn):
    threading.Timer(sec, fn).start()


def scan():
    for i, (mac, d) in enumerate(found):
        def one(mac=mac, d=d):
            if mac in devs:                 # known already: seen again, its signal
                if not devs[mac]["paired"]:
                    say("[\x1b[0;93mCHG\x1b[0m] Device %s RSSI: -%d" % (mac, 60 + i))
                return
            devs[mac] = dict(name=d["name"], icon=d["icon"], paired=False, conn=False, trusted=False)
            say("[\x1b[0;92mNEW\x1b[0m] Device %s %s" % (mac, d["name"] or mac.replace(":", "-")))
            say("[\x1b[0;93mCHG\x1b[0m] Device %s RSSI: -%d" % (mac, 50 + i))
        later(0.6 + i * 0.5, one)


def pair(mac):
    d = devs.get(mac)
    if not d:
        say("Device %s not available" % mac)
        return
    say("Attempting to pair with %s" % mac)

    def ok():
        d["paired"] = True
        say("[\x1b[0;93mCHG\x1b[0m] Device %s Bonded: yes" % mac)
        say("[\x1b[0;93mCHG\x1b[0m] Device %s Paired: yes" % mac)
        say("Pairing successful")
    if d["icon"] == "input-gaming":
        waiting["yes"] = ok
        later(0.8, lambda: ask("\x1b[0;91m[agent]\x1b[0m Confirm passkey 123456 (yes/no): "))
    elif d["icon"] == "input-keyboard":
        later(0.5, lambda: say("\x1b[0;91m[agent]\x1b[0m Passkey: 482917"))
        later(3.5, ok)
    else:
        later(1.5, lambda: say("Failed to pair: org.bluez.Error.ConnectionAttemptFailed"))


def main():
    out.write(P)
    out.flush()
    for raw in sys.stdin:
        cmd = raw.strip().split()
        if not cmd:
            continue
        c, a = cmd[0], (cmd[1] if len(cmd) > 1 else "")
        if c == "yes" and "yes" in waiting:
            waiting.pop("yes")()
        elif c == "agent":
            say("Agent registered")
        elif c == "default-agent":
            say("Default agent request successful")
        elif c == "show":
            for l in ["Controller 00:11:22:33:44:55 (public)", "\tName: x6100", "\tPowered: yes", "\tDiscoverable: no"]:
                say(l, prompt=False)
            out.write(P); out.flush()
        elif c == "devices":
            for m, d in devs.items():
                say("Device %s %s" % (m, d["name"] or m.replace(":", "-")), prompt=False)
            out.write(P); out.flush()
        elif c == "info":
            d = devs.get(a)
            if not d:
                say("Device %s not available" % a)
                continue
            for l in ["Device %s (public)" % a, "\tName: %s" % d["name"] if d["name"] else "\tAlias: %s" % a.replace(":", "-"),
                      "\tIcon: %s" % d["icon"] if d["icon"] else "\tClass: 0x00000000",
                      "\tPaired: %s" % ("yes" if d["paired"] else "no"), "\tTrusted: %s" % ("yes" if d["trusted"] else "no"),
                      "\tConnected: %s" % ("yes" if d["conn"] else "no")]:
                say(l, prompt=False)
            out.write(P); out.flush()
        elif c == "scan":
            if a == "on":
                say("Discovery started")
                say("[\x1b[0;93mCHG\x1b[0m] Controller 00:11:22:33:44:55 Discovering: yes")
                scan()
            else:
                say("Discovery stopped")
        elif c == "pair":
            pair(a)
        elif c == "cancel-pairing":
            say("Pairing cancelled")
        elif c == "trust":
            devs[a]["trusted"] = True
            say("[\x1b[0;93mCHG\x1b[0m] Device %s Trusted: yes" % a)
            say("Changing %s trust succeeded" % a)
        elif c == "connect":
            say("Attempting to connect to %s" % a)
            def con(a=a):
                devs[a]["conn"] = True
                say("[\x1b[0;93mCHG\x1b[0m] Device %s Connected: yes" % a)
                say("Connection successful")
            later(1.0, con)
        elif c == "disconnect":
            say("Attempting to disconnect from %s" % a)
            devs[a]["conn"] = False
            say("[\x1b[0;93mCHG\x1b[0m] Device %s ServicesResolved: no" % a)
            say("Successful disconnected")
            say("[\x1b[0;93mCHG\x1b[0m] Device %s Connected: no" % a)
        elif c == "remove":
            d = devs.pop(a, None)
            say("[\x1b[0;91mDEL\x1b[0m] Device %s %s" % (a, d["name"] if d else ""))
            say("Device has been removed")
        elif c == "power":
            say("Changing power %s succeeded" % a)
            say("[\x1b[0;93mCHG\x1b[0m] Controller 00:11:22:33:44:55 Powered: %s" % ("yes" if a == "on" else "no"))
        else:
            say("Invalid command in menu main: %s" % c)
    os._exit(0)


main()
