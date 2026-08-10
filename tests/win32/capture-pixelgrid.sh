#!/bin/bash
# capture-pixelgrid.sh — sample one app's RENDERED pixels under both win32
# renderers, for the GDI-vs-GDI+ comparison in
# docs/design/win32-gdiplus-renderer.md.
#
#   ./capture-pixelgrid.sh <app> <canvas-id>
#
# Writes /tmp/grid_<app>_{legacy,gdiplus}.txt — packed 0xAARRGGBB, one per
# line — to be scored with score-pixelgrid.py.
#
# Use a STATIC scene. tumbling_cube measures ~2.8 MAE between two runs of
# the SAME renderer, because its rotation phase differs per launch even
# under AETHER_UI_NO_ANIMATION; that noise swamps the signal being measured.
# golden_gallery is static and scores exactly 0.0 run-to-run.
#
# The canvas id is NOT always 1 (golden_gallery's are 5, 6, 8) -- read it
# from GET /widgets first.
export PATH=/mingw64/bin:/usr/bin:/usr/local/bin:$PATH
cd /home/paul/aether-ui || exit 1
APP="${1:-golden_gallery}"
CID="${2:-5}"
for R in legacy gdiplus; do
    taskkill //IM "$APP.exe" //F >/dev/null 2>&1; sleep 1
    AETHER_UI_TEST_PORT=9222 AETHER_UI_HEADLESS=1 AETHER_UI_NO_ANIMATION=1 \
      AETHER_UI_WIN32_RENDERER="$R" "./build/$APP.exe" >"/tmp/${APP}_$R.log" 2>&1 &
    for i in $(seq 1 40); do
        curl -s -o /dev/null --max-time 1 http://127.0.0.1:9222/widgets && break
        sleep 0.5
    done
    sleep 2
    curl -s --max-time 20 "http://127.0.0.1:9222/canvas/$CID/pixelgrid?w=400&h=300&step=8" \
         -o "/tmp/grid_${APP}_$R.txt" 2>/dev/null
    taskkill //IM "$APP.exe" //F >/dev/null 2>&1
done
wc -l "/tmp/grid_${APP}_legacy.txt" "/tmp/grid_${APP}_gdiplus.txt" 2>&1 | head -2
