"""A former test release turned into a page pointing to the main release (its link went round, e.g. on
Facebook): its files go, its text becomes a big notice with a button to the newest release, and the
marker in it keeps publish.py's pruning off it. Run by hand: Build workflow, input moved_tag."""
import json
import os
import subprocess
import tempfile

REPO = os.environ["GITHUB_REPOSITORY"]
TAG = os.environ["MOVED_TAG"]
URL = "https://github.com/%s/releases/latest" % REPO
BADGE_PL = "https://img.shields.io/badge/%E2%9E%9C%20PRZEJD%C5%B9%20DO%20WYDANIA%20G%C5%81%C3%93WNEGO-2ea043?style=for-the-badge"
BADGE_EN = "https://img.shields.io/badge/%E2%9E%9C%20GO%20TO%20THE%20MAIN%20RELEASE-1f6feb?style=for-the-badge"
MOVED = "<!-- moved-to-main -->"

BODY = f"""{MOVED}
# ⚠️ TO WYDANIE ZOSTAŁO PRZENIESIONE

## Wersja testowa (alfa) jest teraz wydaniem głównym. Pobieraj stąd:

[![Przejdź do wydania głównego]({BADGE_PL})]({URL})

### 👉 {URL}

Tam jest najnowsza aplikacja dla Windows, macOS i Linuksa, z emulatorem NES, Doomem
i instrukcją krok po kroku (INSTRUKCJA.html) w każdej paczce.

---

# ⚠️ THIS RELEASE HAS MOVED

## The test (alpha) version is now the main release. Download it here:

[![Go to the main release]({BADGE_EN})]({URL})

### 👉 {URL}

The newest app for Windows, macOS and Linux is there, with the NES emulator, Doom
and a step-by-step guide (INSTRUCTIONS.html) in every package.
"""


def gh(*args):
    return subprocess.run(["gh", *args], capture_output=True, text=True, check=True).stdout


rel = json.loads(gh("api", "repos/%s/releases/tags/%s" % (REPO, TAG)))
for a in rel.get("assets", []):
    gh("api", "-X", "DELETE", "repos/%s/releases/assets/%d" % (REPO, a["id"]))
    print("removed", a["name"])
with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False, encoding="utf-8") as f:
    json.dump({"name": "⚠️ PRZENIESIONE → wydanie główne · MOVED → the main release", "body": BODY,
               "prerelease": True, "make_latest": "false"}, f)
gh("api", "-X", "PATCH", "repos/%s/releases/%d" % (REPO, rel["id"]), "--input", f.name)
print(TAG, "is now a page pointing to", URL)
