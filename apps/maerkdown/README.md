# Maerkdown

A markdown editor in which every word is a first-class object: it has a
measured box, paints itself as glyph geometry, and answers to pointing.

Entering a word — by click or by caret — expands it in place to its full
ascii markdown source (`==highlight==`, `**bold**`), where every
delimiter is a real, editable character; leaving it re-parses and the
word renders styled again. Editing the delimiters *is* restyling.

Styling is geometry throughout, on the vg text-as-paths surface: bold is
the geometric stroker thickening glyph outlines, italic is the path
toolkit shearing them, super/subscript are the same glyphs at 62% on a
shifted baseline. Measurement and painting share one set of font tables,
so the layout cannot disagree with the pixels — and it renders
byte-identically on GTK4, Win32 and AppKit.

## Syntax

Classic: `# ## ###` headings, `- ` bullets, `**bold**`, `*italic*`,
`` `code` ``, ``` fenced code blocks.

Extended (from the [Extended Markdown Syntax](https://github.com/kotaindah55/extended-markdown-syntax)
plugin for Obsidian — see NOTICE): `++insertion++` underline,
`||spoiler||`, `==highlight==`, `^superscript^`, `~subscript~`.

Markers are word-grained: a delimiter pair wraps whole words, so
mid-word forms stay literal.

## Keys

| | |
| --- | --- |
| letters | build the word at the caret; space commits it |
| ← → | move by character; entering a word expands it |
| ↑ ↓ | move by layout line |
| Backspace | delete at the cursor; at a gap, expand the previous word |
| Enter | split the block |
| Escape | commit the word being edited |
| Ctrl+B / Ctrl+I / Ctrl+U | queue bold / italic / underline (applied at commit) |

## Files

- `mdown.ae` — document model (blocks of words, per-word style flags),
  markdown parse/serialize, and the per-word source form.
- `wordflow.ae` — layout, rendering and hit regions: one set of word
  boxes serving all three.
- `maerkdown.ae` — the editor: caret, buffer, keys, toolbar.
- `test_mdown.ae` — model + round-trip unit tests (ci.sh Phase 0).

Driver specs live in `tests/maerkdown/`; run them with
`tests/spec_matrix.sh maerkdown`.
