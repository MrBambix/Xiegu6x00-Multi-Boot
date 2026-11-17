#!/usr/bin/env python3
"""The NES emulator's network helper, run in the lending system's root (OpenX6X00's Python 3,
wpa_supplicant, udhcpc, iw): WiFi, the games' web page, the emulator's update over the air.

    nes_net.py scan | up | down | web | ota-check | ota-get | ota-flash DEVICE

The emulator reads what it says from files in /tmp (net-*.txt, ota-*.txt). The network's
settings stay in /nes-net (the emulator's own partition): wifi.conf, a PSK, never a password."""
import hashlib
import html
import json
import os
import re
import subprocess
import sys
import threading
import time
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# started by the emulator with no PATH of its own: the system's programs (ip, iw, wpa_supplicant) found
os.environ["PATH"] = "/usr/sbin:/usr/bin:/sbin:/bin:" + os.environ.get("PATH", "")
IFACE = "wlan0"
CONF = "/nes-net/wifi.conf"
GAMES = "/games"                     # the card's NES folder (BOOT)
REPO = "TomekR1/Xiegu6x00-Multi-Boot"
ASSET = "nes-x6100-rootfs.ext4.xz"
UPDATE = GAMES + "/.nes-update.ext4.xz"
EXTS = (".nes", ".fds", ".unf", ".unif", ".nsf", ".zip", ".png", ".jpg", ".jpeg", ".bmp")


def say(name, text):
    tmp = "/tmp/%s.tmp" % name
    with open(tmp, "w") as f:
        f.write(text + "\n")
    os.replace(tmp, "/tmp/%s.txt" % name)


def run(*cmd, timeout=30):
    try:
        return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    except Exception as e:  # noqa: BLE001
        return subprocess.CompletedProcess(cmd, 1, "", str(e))


# ---------------------------------------------------------------------------------------------
# WiFi
# ---------------------------------------------------------------------------------------------
def log(*a):
    with open("/tmp/net.log", "a") as f:
        f.write(time.strftime("%H:%M:%S ") + " ".join(str(x) for x in a) + "\n")


def scan_wpa():
    """Looking through wpa_supplicant (works with the vendor's wext drivers as with nl80211):
    {ssid: (signal, secure)}"""
    nets = {}
    os.makedirs("/var/run/wpa_supplicant", exist_ok=True)
    if run("wpa_cli", "-i", IFACE, "ping").stdout.strip() != "PONG":
        with open("/tmp/scan.conf", "w") as f:
            f.write("ctrl_interface=/var/run/wpa_supplicant\nupdate_config=0\n")
        r = run("wpa_supplicant", "-B", "-i", IFACE, "-c", "/tmp/scan.conf", "-D", "nl80211,wext")
        log("scan: wpa_supplicant", r.returncode, (r.stderr or r.stdout).strip()[:200])
        time.sleep(1)
    for _ in range(4):
        r = run("wpa_cli", "-i", IFACE, "scan")
        log("scan: wpa_cli scan", r.stdout.strip()[:80])
        time.sleep(4)
        out = run("wpa_cli", "-i", IFACE, "scan_results").stdout
        for line in out.splitlines()[1:]:
            p = line.split("\t")
            if len(p) >= 5 and p[4].strip():
                try:
                    sig = float(p[2])
                except ValueError:
                    sig = -90.0
                if sig > 0:                 # some drivers give a quality 0..100
                    sig = sig / 2 - 100
                sec = "WPA" in p[3] or "WEP" in p[3] or "RSN" in p[3]
                old = nets.get(p[4])
                if not old or old[0] < sig:
                    nets[p[4]] = (sig, sec)
        if nets:
            break
    return nets


def ensure_iface():
    """The WiFi chip's driver loaded (with Bluetooth off nothing else loads it)"""
    if os.path.exists("/sys/class/net/" + IFACE):
        return
    for mod in ("8723bu", "rtl8xxxu"):
        r = run("modprobe", mod)
        log("modprobe", mod, r.returncode, r.stderr.strip()[:120])
        for _ in range(10):
            if os.path.exists("/sys/class/net/" + IFACE):
                return
            time.sleep(0.5)


def scan():
    ensure_iface()
    r = run("ip", "link", "set", IFACE, "up")
    log("scan: link up", r.returncode, r.stderr.strip()[:120], "| ifaces:", " ".join(os.listdir("/sys/class/net")))
    nets = {}
    for _ in range(2):
        rr = run("iw", "dev", IFACE, "scan", timeout=20)
        out = rr.stdout
        log("scan: iw", rr.returncode, rr.stderr.strip()[:160], "lines", len(out.splitlines()))
        ssid, sig, sec = None, -100.0, False
        for line in out.splitlines() + ["BSS end"]:
            line = line.strip()
            if line.startswith("BSS "):
                if ssid:
                    old = nets.get(ssid)
                    if not old or old[0] < sig:
                        nets[ssid] = (sig, sec)
                ssid, sig, sec = None, -100.0, False
            elif line.startswith("signal:"):
                try:
                    sig = float(line.split()[1])
                except ValueError:
                    pass
            elif line.startswith("SSID:"):
                ssid = line[5:].strip() or None
            elif line.startswith("RSN:") or line.startswith("WPA:"):
                sec = True
        if nets:
            break
        time.sleep(2)
    if not nets:
        nets = scan_wpa()
    log("scan: found", len(nets))
    log("scan: networks", ", ".join("%s (%d dBm%s)" % (n, s, ", secured" if sec else "") for n, (s, sec) in nets.items()))
    lines = ["%d\t%d\t%s" % (s, 1 if sec else 0, n) for n, (s, sec) in sorted(nets.items(), key=lambda x: -x[1][0])]
    say("net-scan", "\n".join(lines) if lines else "")


def save_network():
    """The chosen network from /tmp/net-req.txt (its name, its password): as a PSK"""
    with open("/tmp/net-req.txt") as f:
        ssid = f.readline().rstrip("\n")
        pw = f.readline().rstrip("\n")
    os.remove("/tmp/net-req.txt")
    os.makedirs("/nes-net", exist_ok=True)
    if pw and not 8 <= len(pw.encode()) <= 63:
        raise ValueError("password: 8 to 63 characters")
    if pw:
        out = run("wpa_passphrase", ssid, pw).stdout
        block = "\n".join(l for l in out.splitlines() if not l.strip().startswith("#psk"))
    else:
        block = 'network={\n\tssid="%s"\n\tkey_mgmt=NONE\n}' % ssid.replace('"', "")
    with open(CONF, "w") as f:
        f.write("ctrl_interface=/var/run/wpa_supplicant\n" + block + "\n")
    os.chmod(CONF, 0o600)


def down():
    run("killall", "udhcpc")
    run("killall", "wpa_supplicant")
    run("ip", "addr", "flush", "dev", IFACE)
    say("net-state", "off")


def ip_of():
    out = run("ip", "-4", "addr", "show", IFACE).stdout
    m = re.search(r"inet (\d+\.\d+\.\d+\.\d+)", out)
    return m.group(1) if m else ""


def up():
    if os.path.exists("/tmp/net-req.txt"):
        try:
            save_network()
        except ValueError as e:
            say("net-state", "fail\t%s" % e)
            return
    if not os.path.exists(CONF):
        say("net-state", "fail\tno network chosen")
        return
    ssid = ""
    m = re.search(r'ssid="([^"]*)"', open(CONF).read())
    if m:
        ssid = m.group(1)
    log("up: network", ssid)
    say("net-state", "busy\t%s" % ssid)
    ensure_iface()
    down()
    run("ip", "link", "set", IFACE, "up")
    os.makedirs("/var/run/wpa_supplicant", exist_ok=True)
    r = run("wpa_supplicant", "-B", "-i", IFACE, "-c", CONF, "-D", "nl80211,wext")
    log("up: wpa_supplicant", r.returncode, (r.stderr or r.stdout).strip()[:200])
    if r.returncode:
        say("net-state", "fail\twpa_supplicant: %s" % (r.stderr or r.stdout).strip()[:80])
        return
    state = ""
    for _ in range(40):
        out = run("wpa_cli", "-i", IFACE, "status").stdout
        m = re.search(r"wpa_state=(\w+)", out)
        state = m.group(1) if m else ""
        if state == "COMPLETED":
            break
        time.sleep(0.5)
    log("up: wpa_state", state)
    if state != "COMPLETED":
        down()
        say("net-state", "fail\t%s" % ("wrong password?" if state in ("4WAY_HANDSHAKE", "DISCONNECTED", "SCANNING") else
                                         state or "no answer"))
        return
    run("udhcpc", "-i", IFACE, "-n", "-q", "-t", "8", "-T", "2", timeout=40)
    ip = ip_of()
    log("up: address", ip or "none")
    if not ip:
        say("net-state", "fail\tno address (DHCP)")
        return
    say("net-state", "ok\t%s\t%s" % (ssid, ip))
    if not os.path.exists("/tmp/net-web.pid") and os.fork() == 0:   # the web page, while the network lasts
        with open("/tmp/net-web.pid", "w") as f:
            f.write(str(os.getpid()))
        os.setsid()
        web()
        os._exit(0)


# ---------------------------------------------------------------------------------------------
# The games' web page
# ---------------------------------------------------------------------------------------------
PAGE = """<!doctype html><html lang="pl"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>X6100 NES</title>
<style>
body{margin:0;font-family:system-ui,sans-serif;background:#0d1522;color:#eef3f8}
main{max-width:760px;margin:0 auto;padding:20px}
h1{font-size:24px;margin:0 0 4px}.sub{color:#8d9db2;margin:0 0 18px}
.drop{border:1px solid #2a3a52;background:#111a28;border-radius:14px;padding:14px;margin-bottom:22px}
.bar{display:flex;gap:8px;align-items:center;flex-wrap:wrap;margin-bottom:10px}
.b2{background:#24344d;color:#eef3f8}.hint{color:#8d9db2;font-size:13px;margin-left:6px}
.box{background:#0a101a;border:1px solid #2a3a52;border-radius:10px;height:260px;overflow:auto}
.box tr{cursor:pointer}.box tr.on td{background:#1f5a3c}.empty{color:#8d9db2;text-align:center;padding:30px}
.foot{display:flex;justify-content:space-between;align-items:center;margin:12px 0 0}#cnt{color:#8d9db2}
.drop.on{background:#123524}
button,label.b{background:#34c77b;color:#05140b;border:0;border-radius:10px;padding:10px 16px;
font-weight:700;font-size:15px;cursor:pointer}
table{width:100%%;border-collapse:collapse}td{padding:9px 6px;border-bottom:1px solid #2a3a52}
td.s{color:#8d9db2;text-align:right;white-space:nowrap}
.del{background:#5a1f27;color:#ffc4c7;padding:6px 12px}
#msg{color:#bdf3d6;margin-left:10px}
button:disabled{opacity:.4;cursor:default}
</style></head><body><main>
<h1>Gry NES na X6100</h1><p class="sub">%(n)d gier · wolne %(free)s MB na karcie</p>
<div class="drop" id="drop">
<div class="bar"><label class="b">+ Pliki<input id="f" type="file" multiple hidden></label>
<label class="b">+ Folder<input id="fd" type="file" webkitdirectory multiple hidden></label>
<button class="b2" id="rm" disabled>− Usuń</button><span class="hint">albo upuść tu pliki</span></div>
<div class="box"><table id="q"></table></div>
<p class="foot"><span id="cnt">Pusto</span><button id="go" disabled>Wgraj do emulatora</button></p>
<p id="msg"></p></div>
<h2 style="font-size:18px">Na karcie</h2>
<table>%(rows)s</table>
<script>
const EXT=['.nes','.fds','.unf','.unif','.nsf','.zip','.png','.jpg','.jpeg','.bmp'];
const msg=document.getElementById('msg'),q=document.getElementById('q'),go=document.getElementById('go'),
rm=document.getElementById('rm'),cnt=document.getElementById('cnt');
let queue=[],sel=new Set();
function show(){q.innerHTML='';queue.forEach((f,i)=>{const r=q.insertRow();if(sel.has(i))r.className='on';
r.onclick=e=>{if(!e.ctrlKey&&!e.metaKey&&!e.shiftKey)sel.clear();sel.has(i)?sel.delete(i):sel.add(i);show()};
r.insertCell().textContent=f.webkitRelativePath||f.name;const s=r.insertCell();s.className='s';
s.textContent=Math.ceil(f.size/1024)+' KB'});
if(!queue.length){const c=q.insertRow().insertCell();c.className='empty';c.textContent='Dodaj gry przyciskiem + albo upuść je tutaj'}
const kb=Math.ceil(queue.reduce((a,f)=>a+f.size,0)/1024);
cnt.textContent=queue.length?('Plików: '+queue.length+' · '+kb+' KB'):'Pusto';
go.disabled=!queue.length;rm.disabled=!sel.size}
function add(files){let skip=0;for(const f of files){const n=f.name.toLowerCase();
if(!EXT.some(e=>n.endsWith(e))){skip++;continue}
if(!queue.some(x=>x.name==f.name))queue.push(f)}
msg.textContent=skip?('Pominięte (nie gry): '+skip):'';show()}
rm.onclick=()=>{queue=queue.filter((f,i)=>!sel.has(i));sel.clear();show()};
document.addEventListener('keydown',e=>{if(e.key=='Delete'&&sel.size)rm.onclick()});
go.onclick=async()=>{go.disabled=true;rm.disabled=true;for(let i=0;i<queue.length;i++){const f=queue[i];
msg.textContent='Wgrywam '+(i+1)+' z '+queue.length+': '+f.name+'…';
const r=await fetch('/upload/'+encodeURIComponent(f.name),{method:'PUT',body:f});
if(!r.ok){msg.textContent='Nie udało się: '+f.name+' ('+await r.text()+')';go.disabled=false;return}}
msg.textContent='Wgrane: '+queue.length;queue=[];setTimeout(()=>location.reload(),800)};
for(const id of ['f','fd']){const el=document.getElementById(id);el.onchange=e=>{add(e.target.files);el.value=''}}
const d=document.getElementById('drop');
d.ondragover=e=>{e.preventDefault();d.classList.add('on')};d.ondragleave=()=>d.classList.remove('on');
d.ondrop=e=>{e.preventDefault();d.classList.remove('on');add(e.dataTransfer.files)};
async function del(n){if(!confirm('Usunąć z karty '+n+'?'))return;
await fetch('/delete/'+encodeURIComponent(n),{method:'POST'});location.reload()}
show();
</script></main></body></html>"""


def safe(name):
    name = os.path.basename(urllib.parse.unquote(name))
    if not name or name.startswith(".") or not name.lower().endswith(EXTS):
        return None
    return name


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def reply(self, code, body, ctype="text/plain; charset=utf-8"):
        b = body.encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(b)))
        self.end_headers()
        self.wfile.write(b)

    def do_GET(self):
        files = sorted(f for f in os.listdir(GAMES) if f.lower().endswith(EXTS) and not f.startswith("."))
        rows = "".join('<tr><td>%s</td><td class="s">%d KB</td><td class="s"><button class="del" onclick="del(%s)">Usuń'
                       '</button></td></tr>' % (html.escape(f), (os.path.getsize(os.path.join(GAMES, f)) + 1023) // 1024,
                                                 html.escape(json.dumps(f), quote=True)) for f in files)
        st = os.statvfs(GAMES)
        games = sum(1 for f in files if not f.lower().endswith((".png", ".jpg", ".jpeg", ".bmp")))
        self.reply(200, PAGE % {"n": games, "free": st.f_bavail * st.f_frsize // 1048576, "rows": rows},
                   "text/html; charset=utf-8")

    def do_PUT(self):
        if not self.path.startswith("/upload/"):
            return self.reply(404, "no")
        name = safe(self.path[8:])
        n = int(self.headers.get("Content-Length") or 0)
        if not name or n <= 0 or n > 16 << 20:
            return self.reply(400, "zła nazwa albo rozmiar")
        tmp = os.path.join(GAMES, ".upload-%d.tmp" % threading.get_ident())
        with open(tmp, "wb") as f:
            left = n
            while left:
                chunk = self.rfile.read(min(65536, left))
                if not chunk:
                    break
                f.write(chunk)
                left -= len(chunk)
        if left:
            os.remove(tmp)
            return self.reply(400, "przerwane")
        os.replace(tmp, os.path.join(GAMES, name))
        os.sync()
        self.reply(200, "ok")

    def do_POST(self):
        if not self.path.startswith("/delete/"):
            return self.reply(404, "no")
        name = safe(self.path[8:])
        if name and os.path.exists(os.path.join(GAMES, name)):
            os.remove(os.path.join(GAMES, name))
            os.sync()
        self.reply(200, "ok")


def web():
    try:
        ThreadingHTTPServer(("0.0.0.0", 80), Handler).serve_forever()
    except Exception as e:  # noqa: BLE001
        say("net-web", "fail\t%s" % e)


# ---------------------------------------------------------------------------------------------
# The emulator's update over the air: the newest release's emulator image, checked, written
# ---------------------------------------------------------------------------------------------
def api(url):
    req = urllib.request.Request(url, headers={"User-Agent": "x6100-nes", "Accept": "application/vnd.github+json"})
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.load(r)


def fetch_text(url):
    req = urllib.request.Request(url, headers={"User-Agent": "x6100-nes"})
    with urllib.request.urlopen(req, timeout=30) as r:
        return r.read().decode().strip()


def newest():
    for rel in api("https://api.github.com/repos/%s/releases?per_page=10" % REPO):
        assets = {a["name"]: a for a in rel.get("assets", [])}
        if ASSET in assets and "nes-version.txt" in assets and ASSET + ".sha256" in assets:
            return rel, assets
    return None, None


def ota_check():
    try:
        rel, assets = newest()
        if not rel:
            return say("ota-state", "none\tno release with the emulator")
        ver = fetch_text(assets["nes-version.txt"]["browser_download_url"])
        mine = open("/nes-root/build.txt").read().strip() if os.path.exists("/nes-root/build.txt") else ""
        say("ota-state", "%s\t%s\t%s\t%s" % ("same" if ver.split()[0] == mine.split()[0] else "new", rel["tag_name"],
                                             rel.get("published_at", "")[:10], ver))
    except Exception as e:  # noqa: BLE001
        say("ota-state", "fail\t%s" % str(e)[:100])


def ota_get():
    try:
        rel, assets = newest()
        want = fetch_text(assets[ASSET + ".sha256"]["browser_download_url"]).split()[0]
        a = assets[ASSET]
        req = urllib.request.Request(a["browser_download_url"], headers={"User-Agent": "x6100-nes"})
        h = hashlib.sha256()
        got = 0
        with urllib.request.urlopen(req, timeout=60) as r, open(UPDATE + ".part", "wb") as f:
            while True:
                chunk = r.read(65536)
                if not chunk:
                    break
                f.write(chunk)
                h.update(chunk)
                got += len(chunk)
                say("ota-progress", "%d\t%d" % (got, a["size"]))
        if h.hexdigest() != want:
            os.remove(UPDATE + ".part")
            return say("ota-state", "fail\tthe download is damaged (sha256)")
        os.replace(UPDATE + ".part", UPDATE)
        os.sync()
        say("ota-state", "ready\t%s" % rel["tag_name"])
    except Exception as e:  # noqa: BLE001
        say("ota-state", "fail\t%s" % str(e)[:100])


def ota_flash(dev):
    """The image written over the emulator's partition (its size checked first)"""
    try:
        size = int(open("/sys/class/block/%s/size" % os.path.basename(dev)).read()) * 512
        r = subprocess.run("unxz -c %s | wc -c" % UPDATE, shell=True, capture_output=True, text=True)
        need = int(r.stdout.strip() or 0)
        if not need or need > size:
            return say("ota-state", "fail\tthe image (%d) does not fit the partition (%d)" % (need, size))
        r = subprocess.run("unxz -c %s | dd of=%s bs=1M conv=fsync" % (UPDATE, dev), shell=True,
                           capture_output=True, text=True)
        if r.returncode:
            return say("ota-state", "fail\tdd: %s" % r.stderr.strip()[-80:])
        os.remove(UPDATE)
        os.sync()
        say("ota-state", "flashed")
    except Exception as e:  # noqa: BLE001
        say("ota-state", "fail\t%s" % str(e)[:100])


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    {"scan": scan, "up": up, "down": down, "web": web, "ota-check": ota_check, "ota-get": ota_get}.get(
        cmd, lambda: None)() if cmd != "ota-flash" else ota_flash(sys.argv[2])
