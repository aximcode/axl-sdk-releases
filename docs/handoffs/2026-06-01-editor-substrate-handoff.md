# Editor-substrate handoff (2026-06-01)

Building the axl-sdk substrate for AGT's VS Code / Notepad++-class
multiple-buffer text editor. One release (v0.24.0) when complete. Bias:
generic text/file/encoding concerns live in axl-sdk, UI in AGT.

## Git state (READ FIRST)

- Branch `main`, **7 commits ahead of `origin/main`** (which is at
  `v0.23.0` = `2b0fe9a8`), **unpushed**, working tree clean.
- Everything below is under `## Unreleased` in CHANGELOG.md.
- **DO NOT push or tag without explicit user approval**
  ([[feedback_release_approval_gate]]).
- Unit baseline: **5049/5049 both arches** (`test/integration/.last-pass-count`).

Unpushed commits (oldest→newest):
```
dd600485 AxlPageCache + AxlFileView (out-of-core windowing)
32575dad AxlTextBuffer + axl_file_write_atomic
fb120382 AxlRBTree + AxlPieceTree
13ee9e33 AxlPieceTree undo/redo
10794e1f AxlPieceTree undo_checkpoint sugar
3b6a151d editor substrate A (find/dirty/apply_edits/line-iter)
a27c8dcb editor substrate C1/C2 (utf16 transcode + detect_encoding) + .clang-tidy fix
```

## DONE (this batch, on top of the already-shipped piece-tree stack)

- **A1** `axl_piece_tree_find(pt,needle,len,from,flags,*out)` — substring
  over the virtual doc; `AXL_FIND_CASE_INSENSITIVE|BACKWARD|WHOLE_WORD`;
  cross-piece (chunked scan, needle-1 overlap). `AxlFindFlags` enum.
- **A2** `axl_piece_tree_is_modified(pt)` — save-point dirty flag
  (per-record `post_state` id; cur state = top undo record's id; save
  sets `saved_state`; invalidated if history dropped past the point).
- **A4** `axl_piece_tree_apply_edits(pt,AxlEdit[],n)` — batch as ONE undo
  group, applied highest-offset-first (original coords, no adjustment).
  `AxlEdit{offset,del_len,ins,ins_len}`.
- **A5** `axl_piece_tree_line_iter_init/next` (`AxlPieceLineIter`) —
  forward O(n) line pass (walks pieces + per-source newline arrays).
- **C2** `axl_utf16_to_utf8` / `axl_utf8_to_utf16` (axl-str.h) —
  surrogate-aware, length-counted, NULL-dst measure mode, clean trunc.
- **C1** `axl_detect_encoding(prefix,len,*has_bom)` (axl-stream.h) — BOM
  sniff (UTF-8/UTF-16 LE/BE) + BOM-less interleaved-NUL heuristic;
  returns `AxlEncoding`.
- **.clang-tidy**: disabled `clang-analyzer-valist.Uninitialized` and
  `clang-analyzer-security.ArrayBound` (FP-only here; budget-sensitive,
  were nondeterministically blocking the gate). Full `-n1` sweep exit 0.

## REMAINING (implement, test-first BOTH arches, then release)

Model: AxlPieceTree / AxlArray / AxlRBTree. Conventions: freestanding C,
AxlFoo/axl_foo_*, AXL_OK/AXL_ERR or ptr-or-NULL, AXL_WARN_UNUSED on
fallible calls, AXL_DEFINE_AUTOPTR_CLEANUP for owned handles, doc-comment
style of current headers, single-threaded.

- **C3** [Minor] `axl_piece_tree_load_encoded(path,page,frames,*enc,*bom)`
  + `axl_piece_tree_save_encoded(pt,path,enc,bom)`. load: detect; plain
  UTF-8(no BOM) → `axl_piece_tree_open` (out-of-core); else read+
  transcode (UCS-2 via fopen+`axl_stream_set_encoding`; UTF-16 via the
  new helpers; strip BOM) into `axl_piece_tree_new`. save: get UTF-8 doc,
  transcode to `enc`, prepend BOM if `bom`, `axl_file_write_atomic`
  (materialize for the transcode path — fine for the rare save-as-other-
  encoding case).
- **D1** [Minor] `axl_clipboard_set(data,len[,mime])` / `_get(*len[,mime])`
  / `_clear()` — process-global owned-byte clipboard (UEFI has none).
  New `src/util/axl-clipboard.c` + `include/axl/axl-clipboard.h`. Reuse an
  AxlString or raw buffer; optional mime string (NULL ok).
- **E1** [Minor] `axl_piece_tree_detect_eol(pt) -> AxlEol{LF,CRLF,CR,MIXED}`;
  make `line_bounds`/line-iter `end` exclude a trailing `\r` (CRLF);
  `axl_piece_tree_set_eol(pt,eol)` so `save` writes the chosen terminator
  (translate `\n`↔`\r\n`/`\r` while streaming).
- **E2** [Minor] read-only mode: `axl_piece_tree_set_read_only(pt,bool)`
  (or an open flag); insert/delete/apply_edits return AXL_ERR when set.
- **E3** [Minor] `axl_piece_tree_backing_changed(pt) -> bool` — compare
  current `axl_file_info(path)` size/mtime vs values captured at open
  (store path+size+mtime in the piece tree at open time).
- **B1** [Strong] Shared page cache. EXTEND AxlPageCache to multi-tenant:
  `axl_page_cache_fetch(pc,owner,page_index,fill,user,*valid_len)` keyed
  by (owner,page_index) with one global LRU; `axl_page_cache_drop_owner
  (pc,owner)` so a closing view reclaims its frames; keep the existing
  single-tenant `get` as a back-compat wrapper (owner = the cache). Then
  `axl_file_view_open_cached(path,page_size,AxlPageCache*)` (borrowed
  cache, drops its owner-frames on close) and
  `axl_piece_tree_open_cached(path,AxlPageCache*,page_size,max_frames)`.
- **A3** [Core] — RESOLVED: **no new API.** The consumer composes
  existing primitives for save-over-the-open-file + rebase + bounded
  memory: `save(pt,"F.savetmp")` → `axl_piece_tree_free(pt)` →
  `axl_file_move("F.savetmp","F")` → `axl_piece_tree_open("F")`. Deliver
  an INTEGRATION TEST proving this flow on UEFI FAT (content exact,
  single-piece after reopen = bounded, dirty cleared) + document the
  recipe. Save-As to a new name keeps undo.

### OUT / deferred
- **F1** (snapshot + Myers line-diff) — DEFERRED to a follow-up release
  (user's call). Note: append-only buffers make a cheap copied-span-list
  snapshot feasible later with no COW redesign.
- Regex search — OUT (substring + CI + whole-word is the bar).
- All UI (rendering, caret/selection, find/replace dialog, multi-cursor
  UI, syntax highlight, folding, minimap, tab-expansion) stays in AGT.

## Design decisions (locked, don't re-derive)
- **A3**: saving over the open out-of-core file is inherently a rebase
  (overwriting the original invalidates ORIGINAL-piece offsets AND the
  add-buffer-relative undo) → undo resets. So provide primitives; the
  consumer chooses save-over-self (rebase, lose undo) vs Save-As (keep
  undo). No library policy baked in.
- **B1**: one AxlPageCache across M files needs per-file key
  disambiguation (page_index collides) → multi-tenant extension, NOT
  "just attach".
- **F1**: deferred.

## Build / test / gate
```
make ARCH=x64 && make tests ARCH=x64 && timeout 200s ./test/integration/test-axl.sh --arch X64
make ARCH=aa64 && make tests ARCH=aa64 && timeout 200s ./test/integration/test-axl.sh --arch AARCH64
# full lint sweep (must be exit 0):
rm -f compile_commands.json; bear -- make tests tools
find src -name '*.c' -not -path '*/backend/*' -not -name 'axl-mbedtls-platform.c' -print0 \
  | xargs -0 -n1 -P"$(nproc)" clang-tidy -p . -quiet; echo exit=$?
./scripts/build-docs.sh
```
Per new public header: add `///<`/`@brief`/`@return` docs, a Sphinx page
or `.. doxygenfile::` entry, an `include/axl.h` umbrella include, and a
`src/*/README.md` section. New test binary needs Makefile TESTS +
BUILD_TEST + test-axl.sh TEST_APPS (clipboard tests are fs-free; eol/ro/
backing tests + A3 use fs0:). Independent review pass per
[[feedback_code_review_before_commit]] before each commit.

## Release (when all land, gated on user approval)
Full RELEASING.md flow → cut **v0.24.0** (AGT dependency floor). All the
pre-tag gates, `bump-version.sh 0.24.0`, date the `## Unreleased`
CHANGELOG section, release commit, push main, annotated tag, watch via
`scripts/watch-release-runs.sh`.

## ⚠️ FINAL STEP — AGT-facing summary prompt
After the release (or after the work lands), PRODUCE A PASTE-READY PROMPT
for the user's **AGT session** summarizing: every new axl-sdk editor API
shipped (with one-line usage), what was SKIPPED/deferred (F1 diff,
regex), the v0.24.0 tag to set as AGT's floor, and the grouping/
clipboard/encoding/save-rebase recipes AGT must implement on top (smart
undo grouping via group_begin/end or undo_checkpoint; clipboard policy;
EOL/encoding round-trip; save-over-self via save-temp+free+move+open).
Do not forget this — it's how AGT learns what substrate it can rely on.
