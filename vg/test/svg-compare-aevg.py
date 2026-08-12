#!/usr/bin/env python3
"""
AeVG SVG rendering conformance — side-by-side comparison against librsvg.

For each SVG in the corpus, renders three ways and reports per-pixel MAE
(Mean Absolute Error, 0=identical .. 255=max) of each AeVG column vs librsvg:

  Reference  — librsvg (rsvg-convert), the "correct" answer.
  Loader     — AeVG loader.load_svg → canvas → PNG (the render pipeline).
  Transpiled — SVG → AeVG render fn → batched compile → render → PNG
               (transpiler fidelity). All SVGs' render fns are bin-packed into
               ~14 batch modules compiled once each (vs one compile per SVG).
               There is NO per-file fallback: any emit/compile/render failure
               raises TranspileError and aborts the run (exit 2), so a broken
               transpiler can never produce a green-looking report.

Produces a self-contained HTML report (sortable, worst-first) + CSVs.

Adapted from cosyne/test/svg-compare.py (the TS CVG harness). The corpus is the
same 208-file W3C/CVG set; default location is the cosyne test/svg dir.

Usage:
  python3 aevg/test/svg-compare-aevg.py                 # all SVGs, loader only
  python3 aevg/test/svg-compare-aevg.py 410.svg heart   # specific files
  python3 aevg/test/svg-compare-aevg.py --transpile     # also build+render transpiled
  python3 aevg/test/svg-compare-aevg.py --limit 20      # first 20 (quick smoke)
  python3 aevg/test/svg-compare-aevg.py --svg-dir DIR   # custom corpus dir
"""

import sys
import os
import base64
import html as html_mod
import argparse
import subprocess
import tempfile
from pathlib import Path
from PIL import Image

SCRIPT_DIR = Path(__file__).resolve().parent           # vg/test
AEVG_ROOT = SCRIPT_DIR.parent.parent                   # repo root (aether-ui)
# Post-re-namespace (2026-07): the loader binary is the aeb-built
# svg_render_png app, not the old build.sh `build/svg_render`.
# Probe for the .exe rather than testing os.name: under MSYS2 python reports
# os.name == 'posix' while aeb still emits svg_render_png.exe, so a platform
# test picks the wrong name on exactly the box that needs the suffix.
_RENDER_DIR = AEVG_ROOT / 'target' / 'build' / 'apps' / 'svg_render_png' / 'bin'
RENDER_BIN = (_RENDER_DIR / 'svg_render_png.exe') if (_RENDER_DIR / 'svg_render_png.exe').exists() \
             else (_RENDER_DIR / 'svg_render_png')
BUILD_SH = AEVG_ROOT / 'build.sh'      # only used by the --transpile path (removed; that path is stale)

# Default corpus: the cosyne 208-file W3C/CVG set. Override with --svg-dir.
# MSYS2's python defaults stdout to cp1252, which cannot encode the U+2713
# tick this harness prints per file. The run then dies with UnicodeEncodeError
# on the FIRST SUCCESSFUL render -- i.e. it crashes precisely because things
# started working. Force UTF-8 rather than make every Windows caller remember
# PYTHONIOENCODING.
try:
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    sys.stderr.reconfigure(encoding='utf-8', errors='replace')
except Exception:
    pass

def _default_svg_dir():
    # Path.home() raises RuntimeError when HOME/USERPROFILE are unset, which is
    # exactly the case in a non-interactive `ssh box 'cmd'` shell on MSYS2. It
    # used to be evaluated at import time, so the harness died before argparse
    # could even see a --svg-dir that made it irrelevant.
    try:
        base = Path.home()
    except RuntimeError:
        base = Path(os.environ.get('HOME') or os.environ.get('USERPROFILE') or '.')
    return base / 'scm' / 'tsyne' / 'tsyne' / 'cosyne' / 'test' / 'svg'

DEFAULT_SVG_DIR = _default_svg_dir()
DEFAULT_OUTPUT = SCRIPT_DIR / 'screenshots' / 'svg-compare-aevg'

SIZE = 400


def render_reference(svg_path: Path, output_path: Path):
    """Render SVG with rsvg-convert (librsvg) — the reference. White bg, square."""
    tmp = output_path.with_suffix('.tmp.png')
    subprocess.run(
        ['rsvg-convert', '-w', str(SIZE), '-h', str(SIZE),
         '--keep-aspect-ratio', '-b', 'white', '-o', str(tmp), str(svg_path)],
        check=True, capture_output=True,
    )
    img = Image.open(tmp).convert('RGBA')
    if img.size != (SIZE, SIZE):
        canvas = Image.new('RGBA', (SIZE, SIZE), (255, 255, 255, 255))
        canvas.paste(img, ((SIZE - img.width) // 2, (SIZE - img.height) // 2))
        canvas.save(output_path)
    else:
        img.save(output_path)
    img.close()
    tmp.unlink(missing_ok=True)


def _composite_white(png_path: Path) -> Image.Image:
    """Load a PNG and composite over white (AeVG renders on transparent bg,
    librsvg on white — match them before diffing)."""
    o = Image.open(png_path).convert('RGBA')
    bg = Image.new('RGBA', o.size, (255, 255, 255, 255))
    bg.alpha_composite(o)
    return bg.convert('RGB')


def render_loader(svg_path: Path, output_path: Path) -> bool:
    """Render SVG via the AeVG loader binary: svg_render <in> <out> <size>."""
    if not RENDER_BIN.exists():
        return False
    env = os.environ.copy()
    env['AETHER_UI_HEADLESS'] = '1'
    try:
        r = subprocess.run(
            [str(RENDER_BIN), str(svg_path), str(output_path), str(SIZE)],
            env=env, capture_output=True, timeout=30)
        return r.returncode == 0 and output_path.exists()
    except subprocess.TimeoutExpired:
        print(f'  loader render timed out: {svg_path.name}', file=sys.stderr)
        return False


# Tools for the transpiled column. Post-re-namespace (2026-07) these are aeb
# app nodes under apps/, built to target/build/apps/<name>/bin/<name>.
TRANSPILE_MODULE_BIN = AEVG_ROOT / 'target' / 'build' / 'apps' / 'svg_transpile_module' / 'bin' / 'svg_transpile_module'
TRANSPILE_RENDERFN_BIN = AEVG_ROOT / 'target' / 'build' / 'apps' / 'svg_transpile_render_fn' / 'bin' / 'svg_transpile_render_fn'


def _ensure_built(app: str, bin_path: Path) -> bool:
    """Build an aeb app node if its binary is missing. Returns True if present."""
    if bin_path.exists():
        return True
    r = subprocess.run(['aeb', f'apps/{app}/.build.ae'],
                       cwd=str(AEVG_ROOT), capture_output=True, text=True)
    return bin_path.exists()


def transpile_to_source(svg_path: Path) -> str:
    """Emit the human-facing vg{} module source for display (the 'show source'
    pane). Best-effort; '' if the emitter tool isn't available."""
    if not _ensure_built('svg_transpile_module', TRANSPILE_MODULE_BIN):
        return ''
    try:
        r = subprocess.run([str(TRANSPILE_MODULE_BIN)],
                           input=svg_path.read_bytes(),
                           capture_output=True, timeout=20)
        return r.stdout.decode('utf-8', 'replace') if r.returncode == 0 else ''
    except Exception:
        return ''


def _fn_name(svg_path: Path) -> str:
    """Aether-identifier-safe render-fn suffix for an SVG (stem; digit-led →
    prefixed). Must be unique across the corpus — stems are."""
    s = ''.join(c if (c.isalnum() or c == '_') else '_' for c in svg_path.stem)
    if not s or s[0].isdigit():
        s = 'svg_' + s
    return s


_COMBINED_HEADER = (
    '// Generated COMBINED transpiler-parity harness batch (one compile, N renders).\n'
    'import ui\n'
    'import ui (canvas_create, canvas_write_png)\n'
    'import vg\n'
    'import vg (view_box)\n'
    'import vg.grammar.factories\n'
    'import vg.backend.gtk\n'
    'import vg.svg.loader\n'
    'import std.string\n'
    'extern getenv(name: string) -> string\n'
    'extern exit(code: int)\n\n'
)

# aetherc caps a source file at ~50k tokens. One mega-module for all 208 SVGs
# blows past it, so we bin-pack the emitted render functions into batches under
# a byte budget (a proxy for token count) and compile each batch once. 208 →
# ~14 compiles. Empirically 300KB/batch packs cleanly with zero token-limit
# failures; an emit bigger than the budget on its own seeds its own batch and
# still compiles (the budget governs only what we ADD to a started batch).
_COMBINED_BATCH_BUDGET = 300_000


class TranspileError(Exception):
    """A hard failure in the transpiled column. There is NO per-file fallback —
    a failed emit, batch compile, or render aborts the whole run so a broken
    transpiler can never masquerade as a passing report."""


def _compile_combined_batch(idx: int, entries) -> Path:
    """entries: list of (svg_name, fn_name, body). Emit one batch module +
    dispatch, compile it, return the binary path. Raises TranspileError if the
    batch fails to compile — no fallback."""
    bin_path = AEVG_ROOT / 'build' / f'_gen_batch{idx}'
    gen_ae = AEVG_ROOT / f'_gen_batch{idx}.ae'   # repo root so `import ui`/`vg.*` resolve
    dispatch = [
        f'    if string.equals(which, "{nm}") == 1 '
        f'{{ render_{fn}(out, sz); println("ok"); exit(0) }}'
        for nm, fn, _ in entries
    ]
    main_fn = (
        'main() {\n'
        '    out = getenv("AEVG_OUT")\n'
        '    if string.length(out) == 0 { println("no AEVG_OUT"); exit(2) }\n'
        '    sz = 400\n'
        '    szs = getenv("AEVG_SIZE")\n'
        '    if string.length(szs) > 0 { v, ok = string.to_int(szs); if ok == 1 { sz = v } }\n'
        '    which = getenv("AEVG_WHICH")\n'
        + '\n'.join(dispatch) + '\n'
        '    println("unknown AEVG_WHICH"); exit(3)\n'
        '}\n'
    )
    gen_ae.write_text(_COMBINED_HEADER + '\n'.join(b for _, _, b in entries) + '\n' + main_fn)
    gen_c = AEVG_ROOT / 'build' / f'_gen_batch{idx}.c'
    out = err = ''
    try:
        # aetherc → C, then gcc link against the GTK4 backend (the batch
        # `import ui` + vg canvas need it). Mirrors ci.sh's AEVG_GTK_TESTS.
        ae_cflags = subprocess.run(['ae', 'cflags'], capture_output=True, text=True).stdout.split()
        gtk_cflags = subprocess.run(['pkg-config', '--cflags', 'gtk4'], capture_output=True, text=True).stdout.split()
        gtk_libs = subprocess.run(['pkg-config', '--libs', 'gtk4'], capture_output=True, text=True).stdout.split()
        c = subprocess.run(['aetherc', '--lib', str(AEVG_ROOT), str(gen_ae), str(gen_c)],
                           cwd=str(AEVG_ROOT), capture_output=True, text=True, timeout=600)
        out, err = c.stdout, c.stderr
        if gen_c.exists():
            l = subprocess.run(
                ['gcc', *gtk_cflags, str(gen_c),
                 'backend/aether_ui_gtk4.c', 'backend/aether_ui_system_extras.c', 'backend/aether_ui_sni.c',
                 *ae_cflags, '-pthread', '-lm', *gtk_libs, '-o', str(bin_path)],
                cwd=str(AEVG_ROOT), capture_output=True, text=True, timeout=600)
            out += l.stdout; err += l.stderr
    finally:
        gen_ae.unlink(missing_ok=True)
        gen_c.unlink(missing_ok=True)
    if not bin_path.exists():
        tok = 'maximum token limit' in (out + err)
        why = ('exceeded aetherc token limit — lower _COMBINED_BATCH_BUDGET'
               if tok else 'aetherc/cc error')
        raise TranspileError(
            f'batch {idx} ({len(entries)} SVGs) failed to compile: {why}\n'
            f'{(out + err)[-1500:]}')
    return bin_path


def build_combined_harness(svgs) -> dict:
    """Emit one render_<name>() per SVG, bin-pack them into batch modules under
    a byte budget, and compile each batch ONCE. The per-file aetherc+link is
    ~88% of a --transpile run; ~14 batch compiles instead of 208 per-file
    compiles cuts a full run from ~7min to ~90s.

    Returns {routes: {svg_name: bin_path}} covering EVERY input SVG. There is
    no fallback: a failed emit or a failed batch compile raises TranspileError
    and aborts the run (caller exits nonzero). A broken transpiler must never
    produce a green-looking report."""
    if not _ensure_built('svg_transpile_render_fn', TRANSPILE_RENDERFN_BIN):
        raise TranspileError(
            f'could not build the render-fn emitter ({TRANSPILE_RENDERFN_BIN.name})')

    # 1. Emit every render fn (any failure aborts), recording size for packing.
    emitted = []  # (svg_name, fn_name, body, size)
    for svg in svgs:
        fn = _fn_name(svg)
        try:
            r = subprocess.run([str(TRANSPILE_RENDERFN_BIN)],
                               input=svg.read_bytes(),
                               env={**os.environ, 'AEVG_FN': fn},
                               capture_output=True, timeout=20)
        except Exception as e:
            raise TranspileError(f'{svg.name}: render-fn emit crashed: {e}')
        if r.returncode != 0 or not r.stdout:
            raise TranspileError(
                f'{svg.name}: render-fn emit failed (rc={r.returncode})\n'
                f'{r.stderr.decode("utf-8", "replace")[-800:]}')
        body = r.stdout.decode('utf-8', 'replace')
        if f'render_{fn}(' not in body:
            raise TranspileError(f'{svg.name}: render-fn emit produced no render_{fn}()')
        emitted.append((svg.name, fn, body, len(body)))

    # 2. Greedy bin-pack biggest-first under the byte budget.
    emitted.sort(key=lambda e: -e[3])
    bins = []  # list of {'sz': int, 'entries': [(name, fn, body)]}
    for name, fn, body, sz in emitted:
        placed = False
        for bn in bins:
            if bn['sz'] + sz <= _COMBINED_BATCH_BUDGET:
                bn['entries'].append((name, fn, body)); bn['sz'] += sz; placed = True; break
        if not placed:
            bins.append({'sz': sz, 'entries': [(name, fn, body)]})

    # 3. Compile each batch (any failure aborts); build the full route table.
    routes = {}
    for i, bn in enumerate(bins):
        bin_path = _compile_combined_batch(i, bn['entries'])
        for nm, _, _ in bn['entries']:
            routes[nm] = bin_path
    print(f'  combined: {len(bins)} batches, {len(routes)} SVGs routed', flush=True)
    return {'routes': routes}


def cleanup_combined(routes) -> None:
    """Remove the batch binaries + their .c after the run."""
    for bin_path in set(routes.values()):
        bin_path.unlink(missing_ok=True)
        bin_path.with_suffix('.c').unlink(missing_ok=True)


def render_transpiled_combined(bin_path: Path, svg_name: str, output_path: Path) -> None:
    """Render one SVG via its prebuilt batch binary, selected by AEVG_WHICH.
    Raises TranspileError on any failure — no fallback."""
    env = os.environ.copy()
    env['AETHER_UI_HEADLESS'] = '1'
    env['AEVG_OUT'] = str(output_path)
    env['AEVG_SIZE'] = str(SIZE)
    env['AEVG_WHICH'] = svg_name
    try:
        rr = subprocess.run([str(bin_path)], env=env, capture_output=True, timeout=30)
    except subprocess.TimeoutExpired:
        raise TranspileError(f'{svg_name}: transpiled render timed out')
    if rr.returncode != 0 or not output_path.exists():
        raise TranspileError(
            f'{svg_name}: transpiled render failed (rc={rr.returncode})\n'
            f'{rr.stdout.decode("utf-8", "replace")[-400:]}'
            f'{rr.stderr.decode("utf-8", "replace")[-400:]}')


def png_to_data_uri(png_path: Path) -> str:
    data = png_path.read_bytes()
    return 'data:image/png;base64,' + base64.b64encode(data).decode('ascii')


def pixel_mae(ref_path: Path, other_white: Image.Image) -> float:
    import numpy as np
    ref = np.array(Image.open(ref_path).convert('RGB'), dtype=float)
    oth = other_white
    if oth.size != (ref.shape[1], ref.shape[0]):
        oth = oth.resize((ref.shape[1], ref.shape[0]))
    o = np.array(oth, dtype=float)
    return float(np.mean(np.abs(ref - o)))


def bucket(mae: float) -> str:
    if mae < 0:
        return 'none'
    if mae < 20:
        return 'good'
    if mae < 40:
        return 'ok'
    return 'diff'


def generate_html(results: list[dict], html_path: Path, has_transpile: bool):
    good = sum(1 for r in results if 0 <= r['loader_mae'] < 20)
    ok = sum(1 for r in results if 20 <= r['loader_mae'] < 40)
    bad = sum(1 for r in results if r['loader_mae'] >= 40)
    none = sum(1 for r in results if r['loader_mae'] < 0)

    rows = []
    for r in results:
        m = r['loader_mae']
        b = bucket(m)
        badge = {'good': 'GOOD', 'ok': 'OK', 'diff': 'DIFF', 'none': '?'}[b]
        mae_str = f'MAE {m:.1f}' if m >= 0 else 'no render'

        def panel(label, uri, cls):
            if uri:
                img = f'<img src="{uri}" width="300" height="300">'
            else:
                img = '<div class="no-shot">—</div>'
            return f'<div class="panel"><div class="label {cls}">{label}</div><div class="wrap">{img}</div></div>'

        cols = [
            panel('Reference (librsvg)', r['ref_uri'], 'ref'),
            panel(f'Loader · {mae_str}', r.get('loader_uri'), 'loader'),
        ]
        # Transpiler-gap flag: loader renders well but the transpiled output
        # doesn't (or fails). Isolates a transpiler-fidelity bug from a renderer
        # bug — those are the rows to look at when improving the transpiler.
        tgap = ''
        if has_transpile:
            tm = r['transpiled_mae']
            tlabel = f'Transpiled · MAE {tm:.1f}' if tm >= 0 else 'Transpiled · — (no render)'
            cols.append(panel(tlabel, r.get('transpiled_uri'), 'transpiled'))
            if m >= 0 and m < 20 and (tm < 0 or tm > m + 15):
                tgap = '<span class="badge tgap">TRANSPILER GAP</span>'

        src = html_mod.escape(r.get('svg_source', ''))
        tsrc = html_mod.escape(r.get('transpiled_source', ''))
        src_links = '<a href="#" class="src" onclick="tog(this,\'svg\');return false">svg</a>'
        src_blocks = f'<div class="srcpane" data-k="svg"><pre><code class="language-xml">{src}</code></pre></div>'
        if tsrc:
            src_links += ' <a href="#" class="src" onclick="tog(this,\'ae\');return false">vg{} source</a>'
            src_blocks += f'<div class="srcpane" data-k="ae"><pre><code>{tsrc}</code></pre></div>'

        rows.append(f'''
    <div class="cmp" data-bucket="{b}" data-tgap="{1 if tgap else 0}">
      <div class="hdr"><span class="name">{r['name']}</span>
        <span class="badge {b}">{badge}</span>
        {tgap}
        <span class="links">{src_links}</span></div>
      <div class="imgs">{''.join(cols)}</div>
      {src_blocks}
    </div>''')

    html = f'''<!DOCTYPE html><html><head><meta charset="utf-8">
<title>AeVG SVG Conformance</title>
<style>
 body{{font-family:system-ui,sans-serif;margin:0;background:#f5f5f5}}
 .sticky{{position:sticky;top:0;background:#f5f5f5;padding:16px 20px 8px;border-bottom:1px solid #ddd;z-index:5}}
 h1{{margin:0 0 4px;font-size:20px}} .sum{{color:#666;font-size:14px}}
 .sum .g{{color:#2a2}} .sum .o{{color:#a80}} .sum .d{{color:#c22}}
 .filters{{margin-top:8px;display:flex;gap:6px}}
 .filters input{{padding:6px 10px;border:1px solid #ccc;border-radius:4px;width:200px}}
 .filters button{{padding:6px 12px;border:1px solid #ccc;border-radius:4px;background:#fff;cursor:pointer}}
 .filters button.on{{background:#333;color:#fff}}
 #list{{padding:16px 20px}}
 .cmp{{background:#fff;border-radius:8px;margin-bottom:14px;padding:14px;box-shadow:0 1px 3px rgba(0,0,0,.1)}}
 .hdr{{display:flex;align-items:center;gap:10px;margin-bottom:10px}}
 .name{{font-weight:600}} .links{{margin-left:auto;font-size:12px}}
 .badge{{padding:2px 8px;border-radius:3px;font-size:11px;font-weight:700}}
 .badge.good{{background:#d4edda;color:#155724}} .badge.ok{{background:#fff3cd;color:#856404}}
 .badge.diff{{background:#f8d7da;color:#721c24}} .badge.none{{background:#e2e3e5;color:#383d41}}
 .badge.tgap{{background:#e0d4f8;color:#4a2a72}}
 .imgs{{display:flex;gap:14px;flex-wrap:wrap}}
 .panel{{text-align:center}} .panel img,.no-shot{{width:300px;height:300px;border:1px solid #ddd;background:#fff}}
 .no-shot{{display:flex;align-items:center;justify-content:center;color:#bbb;font-size:24px}}
 .label{{font-size:12px;color:#666;margin-bottom:4px}}
 .ref{{color:#2a2}} .loader{{color:#07a}} .transpiled{{color:#a2a}}
 .src{{color:#888;text-decoration:none;margin-left:6px}} .src:hover{{color:#333;text-decoration:underline}}
 .srcpane{{display:none;text-align:left;background:#282c34;color:#ddd;border-radius:6px;padding:12px;margin-top:10px;max-height:420px;overflow:auto}}
 .srcpane pre{{margin:0}} .srcpane code{{font-family:Menlo,Consolas,monospace;font-size:12px;line-height:1.5}}
</style></head><body>
<div class="sticky">
 <h1>AeVG SVG Conformance</h1>
 <p class="sum">{len(results)} SVGs · <span class="g">{good} good</span>, <span class="o">{ok} ok</span>, <span class="d">{bad} diff</span>, {none} none · loader vs librsvg · MAE 0–255 · worst first</p>
 <div class="filters">
  <input id="q" placeholder="filter by name…" oninput="flt()">
  <button class="on" data-f="all" onclick="setf(this)">All</button>
  <button data-f="diff" onclick="setf(this)">Diff</button>
  <button data-f="ok" onclick="setf(this)">OK</button>
  <button data-f="good" onclick="setf(this)">Good</button>
  <button data-f="tgap" onclick="setf(this)">Transpiler gap</button>
 </div>
</div>
<div id="list">{''.join(rows)}</div>
<script>
var cf='all';
function setf(b){{cf=b.dataset.f;document.querySelectorAll('.filters button').forEach(x=>x.classList.remove('on'));b.classList.add('on');flt();}}
function flt(){{var q=document.getElementById('q').value.toLowerCase();
 document.querySelectorAll('.cmp').forEach(function(e){{
  var n=e.querySelector('.name').textContent.toLowerCase();
  var match = cf=='all' || (cf=='tgap' ? e.dataset.tgap=='1' : e.dataset.bucket==cf);
  var ok=match&&(!q||n.includes(q));
  e.style.display=ok?'':'none';}});}}
function tog(a,k){{var c=a.closest('.cmp');var p=c.querySelector('.srcpane[data-k="'+k+'"]');
 var open=p.style.display=='block';c.querySelectorAll('.srcpane').forEach(x=>x.style.display='none');
 p.style.display=open?'none':'block';}}
</script></body></html>'''
    html_path.write_text(html)


def main():
    ap = argparse.ArgumentParser(description='AeVG SVG conformance vs librsvg')
    ap.add_argument('files', nargs='*', help='specific SVGs (default: all)')
    ap.add_argument('--svg-dir', type=Path, default=DEFAULT_SVG_DIR)
    ap.add_argument('--output', type=Path, default=DEFAULT_OUTPUT)
    ap.add_argument('--limit', type=int, default=0, help='only first N (smoke)')
    ap.add_argument('--transpile', action='store_true', help='also build+render transpiled column')
    ap.add_argument('--no-open', action='store_true')
    args = ap.parse_args()

    if not RENDER_BIN.exists():
        print(f'Render binary missing: {RENDER_BIN}\n'
              f'Build it:  ./build.sh aevg/svg_render_png.ae build/svg_render', file=sys.stderr)
        return 1

    svg_dir = args.svg_dir
    out = args.output
    out.mkdir(parents=True, exist_ok=True)

    if args.files:
        svgs = []
        for f in args.files:
            p = Path(f)
            if not p.exists():
                p = svg_dir / (f if f.endswith('.svg') else f + '.svg')
            svgs.append(p)
    else:
        svgs = sorted(svg_dir.glob('*.svg'))
    if args.limit > 0:
        svgs = svgs[:args.limit]
    if not svgs:
        print('No SVGs found'); return 1

    # Build the batched combined transpiled harness once: one aetherc+link per
    # ~300KB batch (~14 for the corpus) instead of one per SVG (~88% of a
    # --transpile run). There is NO per-file fallback — any emit/compile/render
    # failure raises TranspileError and aborts, so a broken transpiler can never
    # produce a green-looking report.
    combined = {'routes': {}}
    if args.transpile:
        print(f'Building combined transpiled harness ({len(svgs)} renders, batched compiles)…',
              flush=True)
        try:
            combined = build_combined_harness(svgs)
        except TranspileError as e:
            print(f'\nTRANSPILE ABORT: {e}', file=sys.stderr)
            return 2

    results = []
    for i, svg in enumerate(svgs):
        name = svg.name
        print(f'[{i+1}/{len(svgs)}] {name}', end=' ', flush=True)
        ref = out / f'{svg.stem}_ref.png'
        try:
            render_reference(svg, ref)
        except Exception as e:
            print(f'ref FAIL: {e}'); continue

        loader_uri = None
        loader_mae = -1.0
        lpng = out / f'{svg.stem}_loader.png'
        if render_loader(svg, lpng):
            loader_uri = png_to_data_uri(lpng)
            try:
                loader_mae = pixel_mae(ref, _composite_white(lpng))
            except ImportError:
                pass

        transpiled_uri = None
        transpiled_mae = -1.0
        transpiled_source = transpile_to_source(svg) if args.transpile else ''
        if args.transpile:
            tpng = out / f'{svg.stem}_transpiled.png'
            # Every SVG must route to a batch binary; render via AEVG_WHICH.
            # A missing route or a render failure aborts — no fallback.
            route = combined['routes'].get(svg.name)
            if route is None:
                cleanup_combined(combined['routes'])
                print(f'\nTRANSPILE ABORT: {svg.name} has no batch route', file=sys.stderr)
                return 2
            try:
                render_transpiled_combined(route, svg.name, tpng)
            except TranspileError as e:
                cleanup_combined(combined['routes'])
                print(f'\nTRANSPILE ABORT: {e}', file=sys.stderr)
                return 2
            transpiled_uri = png_to_data_uri(tpng)
            try:
                transpiled_mae = pixel_mae(ref, _composite_white(tpng))
            except ImportError:
                pass

        tag = ('?' if loader_mae < 0 else '✓' if loader_mae < 20
               else '~' if loader_mae < 40 else '✗')
        print(f'{tag} {loader_mae:.1f}' if loader_mae >= 0 else '? no-render')

        results.append({
            'name': name,
            'loader_mae': loader_mae,
            'transpiled_mae': transpiled_mae,
            'ref_uri': png_to_data_uri(ref),
            'loader_uri': loader_uri,
            'transpiled_uri': transpiled_uri,
            'svg_source': svg.read_text(encoding='utf-8', errors='replace'),
            'transpiled_source': transpiled_source,
        })

    cleanup_combined(combined['routes'])

    results.sort(key=lambda r: r['loader_mae'] if r['loader_mae'] >= 0 else 999, reverse=True)

    html_path = out / 'comparison.html'
    generate_html(results, html_path, has_transpile=args.transpile)

    csv_path = out / 'results.csv'
    with open(csv_path, 'w') as f:
        f.write('file,loader_mae,loader_status,transpiled_mae\n')
        for r in results:
            f.write(f'{r["name"]},{r["loader_mae"]:.1f},{bucket(r["loader_mae"])},{r["transpiled_mae"]:.1f}\n')

    # Checked-in conformance snapshot (alpha-sorted) for full runs.
    if not args.files and args.limit == 0:
        conf = SCRIPT_DIR / 'svg-conformance-aevg.csv'
        with open(conf, 'w') as f:
            f.write('file,loader_mae,status\n')
            for r in sorted(results, key=lambda r: r['name'].lower()):
                f.write(f'{r["name"]},{r["loader_mae"]:.1f},{bucket(r["loader_mae"])}\n')
        print(f'Conformance snapshot: {conf}')

    good = sum(1 for r in results if 0 <= r['loader_mae'] < 20)
    okc = sum(1 for r in results if 20 <= r['loader_mae'] < 40)
    bad = sum(1 for r in results if r['loader_mae'] >= 40)
    none = sum(1 for r in results if r['loader_mae'] < 0)
    print(f'\n{"="*46}')
    print(f'{len(results)} SVGs · {good} good · {okc} ok · {bad} diff · {none} none')
    print(f'HTML: {html_path}')
    print(f'CSV:  {csv_path}')

    if not args.no_open:
        os.system(f'xdg-open "{html_path}" 2>/dev/null &')
    return 0


if __name__ == '__main__':
    sys.exit(main() or 0)
