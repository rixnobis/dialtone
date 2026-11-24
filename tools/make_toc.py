#!/usr/bin/env python3
# Writes the disc layout for pcsx-redux's authoring tool: toc.json plus every file under assets/VOICE
# when the voice pack is there (tools/pack_voices.py).
#
#   python3 -I tools/make_toc.py > toc-full.json
#   authoring -o build/dialtone-disc.bin toc-full.json     (paths are relative to the toc)
import json, os

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
toc = json.load(open(os.path.join(root, "toc.json")))
voice = os.path.join(root, "assets", "VOICE")
if os.path.isdir(voice):
    for f in sorted(os.listdir(voice)):
        toc["files"].append({"path": f"assets/VOICE/{f}", "name": f"VOICE/{f}"})
print(json.dumps(toc, indent=4))
