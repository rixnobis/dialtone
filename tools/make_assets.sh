#!/bin/sh
# Rebuilds every asset: the Blender desk -> OBJs -> meshbins -> the archive image a PCDRV
# build reads (build/dialtone.bin). For a real disc, run pcsx-redux's authoring tool on toc.json.
set -e
cd "$(dirname "$0")/.."
blender -b --factory-startup -P tools/build_desk.py
mkdir -p assets/MODELS
for obj in build/obj/*.obj; do
	name=$(basename "$obj" .obj)
	python3 -I madnight_engine/tools/obj-to-meshbin.py "$obj" "assets/MODELS/$name.MB" 128 > /dev/null
done
python3 -I madnight_engine/tech-demo/tools/make_image.py assets build/dialtone.bin
