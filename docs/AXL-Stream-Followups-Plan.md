# Plan — AxlStream follow-ups and the pre-existing issues this session surfaced

**Status:** COMPLETE 2026-08-01. Base `9422e2f9`. All nine tasks landed;
what each one actually found is in its own section. Two items were
deliberately not done and are recorded rather than dropped: `tools/` is not
clang-tidy gated (two of its three findings are intentional in `sed.c`), and
only `axl_bufopen` migrated onto the public constructor -- `axl_fopen`, the
text wrapper and the compress writer still construct privately (design S9).

> **Both deferred items are now done** (2026-08-01). The remaining built-ins
> migrated -- design doc S13, and the one thing that could NOT be expressed
> publicly is recorded there rather than worked around, namely that a wrapper
> cannot read the stream it wraps below that stream's transcode. And `tools/`
> is clang-tidy gated, at the same `bugprone-*` scope `test/unit/` uses: the
> `sysinfo.c` finding was a real if trivial omission and is fixed, the two
> `sed.c` clones carry targeted NOLINTs with their reasons written out, and
> the scope table in docs/RELEASING.md now records the measurement.

Seven items, all verified real before being listed. Ordered by risk: silent
wrong answers first, then the gaps, then optimality and housekeeping. Each is
its own commit and must be green on both arches before the next starts.

---

## Task 1 — the Layer-2 helpers lie about short transfers — DONE (`f5bde273`)

> Landed 2026-07-31. Both helpers loop; `fwrite` stops at the first backend
> zero (retrying would spin) and that rule is contractual; `size * count`
> overflow refused; `pread`/`pwrite` set `err` but never `eof`; `axl_fclose`
> guards the five statics and resets them to their ground state. `tools/tr.c`
> moved to `axl_read` so a filter is not made to block until 4 KiB arrives.
> 8948 -> 9021 assertions, both arches. Line numbers below are PRE-fix and
> have since shifted; the other tasks' references need re-checking too.

`axl_fread` (`src/stream/axl-stream.c:1148`) issues **one** `axl_read` and
returns `n / size`. `axl_fwrite` (`:1160`) is identical. Neither loops, though
both are documented "Like fread()/fwrite()" and C's have always looped.

Latent against the old closed backend set, because none of them short-transfer.
`axl_stream_open_custom` makes short transfers contractually legal, so a socket
or ring-buffer backend now produces a short item count that the caller cannot
distinguish from EOF. Design §8b.

Two more coherence fixes in the same file, same commit:

- **`axl_pread`/`axl_pwrite` never set the sticky flags** (`:1119`, `:1130`),
  while `axl_read`/`axl_write` do. Set `err` on a negative return. Deliberately
  do NOT set `eof` on a 0-length positional read: positional I/O has no stream
  position, so "the stream is at end" is not what a short `pread` means.
  Document that asymmetry rather than leaving it to be rediscovered.
- **`axl_fclose` calls `axl_free(s)` unconditionally**, including on the five
  static console streams (`&mStdout` et al. at `:424`). Nothing in-tree does
  it, but nothing stops a consumer either, and freeing a static is heap
  corruption. Guard it.

**Tests:** short-transfer looping for both helpers via a custom backend that
accepts N bytes per call; `err` set after a failed `pread`/`pwrite`; `eof`
NOT set by a short `pread`; `axl_fclose(axl_stdout)` is a safe no-op and
`axl_stdout` still works afterwards.

## Task 2 — no public codepoint-to-UTF-8 encoder — DONE

`axl_utf8_decode` turns bytes into a codepoint; nothing public does the
reverse. `axl_ucs2_to_utf8` converts whole UCS-2 strings, not one codepoint.
The gap is felt: `src/data/axl-json-parse.c:100` carries a private
`utf8_encode(uint32_t cp, char *buf)` reimplementing exactly it.

This also refutes the claim shipped in `157dc321` and design §3.3 that
"axl-str.h has no asymmetric conversion left" — correct that text.

Bucket A. Add `axl_utf8_encode`, then **delete the private copy and make
`axl-json-parse.c` use the public one** — the migration is the proof the API
is right.

**Tests:** ASCII, 2/3/4-byte sequences, the surrogate range (rejected), above
U+10FFFF (rejected), buffer-too-small, and a decode/encode round-trip over the
full BMP.

> Landed 2026-07-31. `axl_utf8_encode(codepoint, dst, dst_size)` +
> `AXL_UTF8_MAX_LEN`. NULL `dst` measures (matching `axl_utf16_to_utf8`, not
> `axl_ucs2_to_utf8_buf`, which is the one sibling where NULL means "give up").
> Surrogates and anything above U+10FFFF are REFUSED rather than encoded as
> CESU-8/WTF-8, and a sequence that will not fit is refused whole — both return
> 0, so 0 always means "nothing was written". 9021 -> 9070 assertions (+49),
> both arches. `.last-pass-count.tls` is deliberately left at 8964: that
> configuration was not run, and the ratchet only fails on a DROP, so it
> self-corrects on the next green `AXL_TLS=1` run rather than being asserted
> blind here.
>
> **Two things worth carrying forward.**
>
> The contract-first review caught a bug in the docstring's own worked idiom:
> it substituted U+FFFD for an unencodable codepoint and then *skipped* the
> fit check, so a substitution that did not fit wrote nothing, did not stop
> the loop, and kept appending the codepoints after it — losing a character
> out of the MIDDLE of otherwise well-formed output. Docstring idioms get
> copy-pasted, so that is shipped code. The corrected idiom re-measures after
> substituting, and `utf8_enc_idiom` in `test/unit/axl-test-string.c` executes
> it verbatim so it cannot rot back.
>
> The plan's claim that "`check-dogfood` exists for exactly this" was wrong:
> `scripts/check-dogfood.py` gates raw `gBS->`/protocol calls and raw
> `AllocatePool`, not re-implementations of `axl_str*`. Nothing automated
> would have caught the private `utf8_encode`, and nothing would catch the
> next one. Left as-is; noted so the next plan does not lean on it.
>
> **Note for the `json-flag-redesign` worktree** (16 unpushed commits): the
> migration below WAS done on main, and it will collide with your rewrite.
> Measured with a real 3-way merge (`git merge-file`) before committing:
> without the migration your branch merges cleanly; with it you get **2
> conflict hunks**, both trivially resolved in your favour (the `size_t n;` /
> `bool substitute` declaration, and the old call site vs your
> `append_scalar` / `append_replacement` dispatch). The trap is what is NOT
> marked: main deletes the static `utf8_encode` and that deletion merges
> silently, so after resolving both hunks your `append_scalar` still calls a
> function that no longer exists. Fix is one line —
> `size_t n = axl_utf8_encode(cp, enc, sizeof(enc));` with
> `char enc[AXL_UTF8_MAX_LEN];` — and then `append_scalar` becomes your single
> adoption point, which is where it belongs on your branch anyway.

## Task 2a — JSON5 `\0` / `\x00` still smuggle an interior NUL — DONE

Surfaced by Task 2's pre-commit review, **not fixed there** — it is a
different feature (JSON5 escape decoding), it needs its own test-first
cycle, and it lands in the same region the `json-flag-redesign` worktree has
rewritten, so folding it into Task 2 would have doubled that merge cost for
unrelated work. Recorded here rather than narrowed out of the docs.

`src/data/axl-json-parse.c`, `decode_json_string`: the `\uXXXX` arm refuses
an escaped U+0000 and substitutes U+FFFD, with a long comment explaining that
an interior NUL truncates the value for every `axl_strcmp` reader — an
`"admin\0extra"` claim compares equal to `"admin"`. Two arms directly above it
do not apply that rule:

- `case '0':` writes `'\0'` unconditionally.
- `case 'x':` writes `(hi << 4) | lo`, so `\x00` writes the same byte.

Strict JSON never reaches either (jsmn's lexer whitelists only
`" / \ b f r n t u` and errors on anything else), so this is JSON5-only. That
is not a safety argument: JSON5 sidecars are user-replaceable via `--ids-file`,
which the number-overflow comment in the same file already cites as the reason
that input is untrusted.

**Fix:** substitute `JSON_UTF8_REPLACEMENT` in both arms, exactly as the
`\uXXXX` arm does. **Tests:** bucket D — an exact-string regression test per
arm asserting the U+FFFD bytes, confirmed RED first.

> Landed 2026-08-01. Both arms substitute U+FFFD, and all three spellings of
> an escaped NUL now go through **one** appender (`append_codepoint`) — the
> `\uXXXX` arm was refactored onto it in the same change, so the three cannot
> diverge on the buffer bound either. That bound is the part the plan did not
> anticipate: the replacement is 3 bytes where the escape used to write 1, so
> these arms newly have to obey the all-or-nothing rule, and a partial write
> would be the exact ill-formed UTF-8 the substitution exists to prevent.
>
> 16 assertions, **10 RED / 6 green-on-arrival**, and the split is the point.
> The 6 pin what the substitution must NOT break: `\x21`, `\x41\x42`, `\x7f`
> and `\x30` still write their bytes (only the ZERO byte is special-cased),
> plus the two truncation boundaries that were already passing for the wrong
> reason. Three sabotages, each precise and disjoint:
>
> | sabotage | failures |
> |---|---|
> | `case '0'` writes a raw NUL again | exactly the 5 `\0` assertions |
> | the `\x` arm stops special-casing zero | exactly the 5 `\x00` assertions |
> | `append_codepoint` ignores the fit check | the 2 pre-existing `\u` bound assertions + the 2 new ones |
>
> The third is what proves the 2 green-on-arrival bound assertions are real
> rather than tautological: they pass today because the pre-fix arm wrote one
> byte, and they pass after the fix because the replacement is refused whole —
> different reasons, and only a partial-write implementation fails them.
>
> **Merge cost against `json-flag-redesign`, measured not estimated**
> (`git merge-file --diff3`, branch head `3b58c6ee`, merge-base `31ce6fe7`):
> main WITHOUT this fix already conflicts in **2** hunks (Task 2's `size_t n;`
> declaration and its `\u` call site). WITH it, **5** — three more: the
> helper block, `case '0'`, and `case 'x'`.
>
> **All three resolve by taking the branch's side unchanged, because the
> branch has ALREADY fixed this bug** — `3b58c6ee` carries
> `append_replacement`, calls it from both arms, and its docstring names the
> same `"admin\0extra"` smuggling primitive. It also goes further, and that
> part is worth pulling back the other way rather than losing: it reads
> `\xNN` as ES5's code UNIT U+00NN, so `\xff` becomes two UTF-8 bytes instead
> of a lone 0xFF — a *second*, unrelated ill-formed-output bug in the same
> arm that this task did not cover and main still has. Deliberately not fixed
> here: it is a different defect needing its own cycle, and duplicating it on
> main would only add conflict surface to a fix that already exists on the
> branch. The four `\xNN` assertions added here are all ASCII, so they stay
> green under either semantics.
>
> One shape correction came out of the measurement and is worth carrying
> forward, because it is the inverse of the trap Task 2 recorded. The first
> version of the `case 'x'` fix put `i += 2;` *before* the new `if (byte ==
> 0)` block, which left that block OUTSIDE the conflict region as common
> context — so a take-theirs resolution silently RETAINED 11 of my lines
> calling an `append_codepoint` the same resolution had just deleted. Task 2's
> trap was a silent deletion; this one was a silent retention. Restructuring
> so the whole change replaces the single base line theirs also replaces made
> the resolution mechanical: the take-theirs output is now byte-identical to
> the without-this-fix resolution except for the leading whitespace of one
> closing brace, and it carries no dangling reference. Verified by generating
> both resolutions with `git merge-file --ours` and diffing them.
>
> The worktree was never touched — every measurement is `git show
> worktree-json-flag-redesign:<path>` into a scratch directory.

## Task 2b — five more private UTF-8 encoders, one of them unguarded — DONE

Task 2 migrated `axl-json-parse.c` only. Still open-coding the encode ladder:
`src/data/axl-xml-parse.c`, `src/vterm/axl-vterm.c`,
`src/util/axl-console-emit.c`, `src/util/axl-console-term.c`,
`src/stream/axl-stream.c`.

Take `axl-vterm.c` first, and treat it as a bug rather than tidying: it is a
byte-identical copy of the ladder with **no** surrogate and **no** range
guard, so a codepoint in U+D800..U+DFFF arriving from libvterm is written out
as 3-byte WTF-8 — precisely the ill-formed output `axl_utf8_encode` exists to
refuse. (`axl-xml-parse.c` already has both guards and is a clean swap.)

Note `axl_ucs2_to_utf8_buf` is deliberately NOT on this list: a UCS-2 string
can legitimately hold unpaired surrogate code *units*, and it currently
encodes them. Routing it through `axl_utf8_encode` would silently change that
to "stop at the surrogate" — a behaviour change needing its own decision.

> Landed 2026-08-01. `a10cbe98` took `axl-vterm.c` ahead of this task, so four
> remained; **three migrated, one refused**, and the refusal is the finding.
>
> | site | same operation? | classification | outcome |
> |---|---|---|---|
> | `axl-xml-parse.c` | yes | duplication — both guards already present | migrated (`55a2bc59`) |
> | `axl-console-emit.c` | yes | **defect** — no surrogate guard, WTF-8 onto a contractually-UTF-8 op | migrated + U+FFFD (`2bc6d300`) |
> | `axl-console-term.c` | yes | **defect** — same, into the cell the renderer and the clipboard read | migrated + U+FFFD (`c0d286ee`) |
> | `axl-stream.c` | **no** | code UNIT → UTF-8 with a published permissive contract | NOT migrated; pinned (`bdf9aa3a`) |
>
> **The plan's own framing was wrong on two counts, both worth correcting.**
>
> It called `axl-xml-parse.c` "a clean swap" and it was — but the swap
> incidentally fixed something the plan did not see. The ladder appended a byte
> at a time into a growable scratch, so an allocation failure partway through a
> multi-byte sequence left a TRUNCATED sequence behind: the same ill-formed
> UTF-8 the surrogate guard exists to prevent, arriving through the OOM path.
> Growing once for the whole sequence makes it all-or-nothing. The plan's "has
> both guards" check was a check on the *refusal* logic and it missed the
> *assembly* logic, which is where this class of bug actually lives.
>
> And listing `axl-stream.c` alongside the others conflated two different
> operations. `encode_utf8_bmp` takes a UCS-2 code UNIT off the wire, not a
> Unicode scalar, and an unpaired surrogate half is representable there —
> `axl-stream.h` and `src/stream/README.md` both publish the round-trip as a
> promise. `axl_utf8_encode` refusing surrogates is *correct for a scalar* and
> would here turn a round-trippable code unit into a dropped one. It is the
> `axl_ucs2_to_utf8_buf` case the paragraph above already excluded, and
> stronger, because this one is a documented contract rather than an
> undocumented current behaviour. Nothing tested it, so the decision is now
> pinned by three assertions whose failure message says why — sabotage-verified
> by *applying* the declined migration, which fails exactly the read-back
> assertion.
>
> **Bug vs. duplication changed the test discipline per site**, which is the
> reason the classification step was worth doing before touching anything: 5
> assertions confirmed RED (bucket D, the two console sites), 10
> green-on-arrival protection (bucket C). The protection is not padding — the
> 2- and 3-byte arms of BOTH console encoders and of the XML decoder had no
> coverage at all, so a botched encode of anything above U+007F would have gone
> unnoticed by all three binaries. Four sabotages, each precise and disjoint;
> the pair worth recording is the two on `axl-console-emit`, because dropping
> the surrogate instead of substituting fails exactly the three byte-level
> assertions while the cell-count assertion survives — it pins the OTHER half
> of the invariant (`track_cursor` advances a column for the same code unit),
> and only sabotaging `track_cursor` shows it can fail at all.
>
> One harness trap cost a false sabotage result: `sed -i.bak` + `mv` to restore
> gives the source file the backup's OLDER mtime, so `make` skips the rebuild
> and the PREVIOUS sabotage's object stays linked. It surfaced as a sabotage in
> `axl-console-term.c` appearing to break an assertion in the tap. `touch` the
> file after any restore, and treat a sabotage that fails something
> topologically unrelated as a contaminated build until proven otherwise.
>
> 9175 -> 9190 assertions, both arches.

## Task 3 — the scanf field-width parser overflows — DONE

`src/data/axl-str-scan.c:411-415` accumulates `max_width = max_width * 10 +
digit` with no guard, so `%18446744073709551617lf` wraps to width 1. Shared by
`%c`, `%s`, `%[` and the float conversions.

Memory-safe today (the wrapped value is never LARGER than what was written, and
every consumer clamps against the input length) but silently wrong. Pre-existing
(`9b576e6d`, 2026-05-07).

**Tests:** one per conversion family, since the guard is shared. An
over-`SIZE_MAX` width must be a malformed-format `-1`, not a wrapped width.

> Landed 2026-07-31. The guard is `max_width > (SIZE_MAX - digit) / 10`,
> checked BEFORE each accumulate, and it covers the integer conversions too
> — the plan named four families, there are five. 9070 -> 9084 assertions
> (+14), both arches.
>
> The count splits 8 RED / 6 green-on-arrival, and the split is the point.
> The 8 are one per family plus the wrap-to-0 variant (`SIZE_MAX + 1`), a
> 26-digit run, and a malformed width sitting AFTER a successful `%d`. The 6
> pin what the guard must not break: a width of exactly `SIZE_MAX` is
> accepted by all four non-float families, and the float cap is pinned at
> both 256 and 257 so the size_t guard cannot be mistaken for the one doing
> that work. Two sabotages confirmed the split is real — `>` to `>=` fails
> only the 4 `SIZE_MAX`-boundary assertions, and `-1` to `return conversions`
> fails only the 8.
>
> `-1` discards conversions the same call already stored, which is worth
> stating rather than discovering: `%d %<huge>s` returns -1 with the `%d`'s
> value already written through the caller's pointer. That is not new and not
> specific to this guard — it is what `%s` without a width, `%llf`, and every
> other malformed-format path here have always done, and C99 leaves a
> malformed format undefined. Making the width guard return the partial count
> instead would have been the inconsistency. Now stated in the header.

## Task 4 — `axl_dtoa` is more approximate than it needs to be — DONE

`kPow10` is `uint32_t[10]`, so the clamp added in `f23e571b` skips the
rounding refinement for the ~70% of conversions whose index exceeds the table.
Widening to `uint64_t` (10^0..10^19) removes the clamp entirely and lifts the
optimally-rounded share from ~72% to ~99.9% over 2M doubles.

Round-trip is already exact either way, so this buys shortest-digit optimality,
not correctness — which is why it was deferred, and why it needs a
differential check rather than a unit assertion.

**Tests:** existing round-trip suites must stay green; add a differential
sweep proving no round-trip regression and measuring the optimality gain.

> Landed 2026-07-31. `kPow10` is `uint64_t[20]` (10^0..10^19) and the clamp
> is gone, not merely raised. Differential host sweep over 2,013,685 doubles
> (2,000,000 random bit patterns + a boundary corpus: DBL_MAX/MIN/TRUE_MIN,
> every power of ten, 4096 subnormals, the neighbourhoods of 1.0 and of the
> normal/subnormal seam), same harness both sides:
>
> | over the whole 2,013,685 | before | after |
> |---|---|---|
> | round-trip failures | 0 | 0 |
> | correctly rounded at its own length | 72.54% | 99.94% |
> | longer than the true minimum | 0.081% | 0.081% |
>
> (`axl-dtoa.c`'s own comments quote the random-bit-pattern subset alone —
> 72.40% and 0.082% — because that is the corpus the pre-existing figures
> in that file were measured on. Same measurement, different slice.)
>
> The third row not moving is the point: refinement walks the last digit, it
> never removes one, so this bought ROUNDING and not LENGTH. `1e23` still
> comes out `"9999999999999999"`.
>
> **The overflow question the widening raises is settled by two bounds, not
> by the sweep.** `one.f = 1 << -Mp.e` with `Mp.e` in `[-60, -34]` for every
> double — established by running `get_cached_power` over the whole closed
> range of `w_p.e` (`[-1137, 960]`), not sampled. From `one.f <= 2^60`:
> `delta` at the refine call is `< 10 * one.f <= 1.15e19`, and
> `wp_w.f <= delta0` makes the product at most that same `delta`, so it
> cannot reach `UINT64_MAX = 1.84e19`. 128-bit instrumentation over
> 44,000,000 conversions agrees: largest delta 1.146e19, largest product
> 6.66e18, zero overflows. The same bound caps the index at 16, three
> entries short of the end, so **the clamp is dead code and was removed
> rather than re-bounded**.
>
> Four pinned digit strings changed, all from a non-optimal string to the
> correctly-rounded one of the same length: `4.7112871036659575e+180`
> `…579` -> `…575` (the existing index-16 pin), plus three new pins at
> indices 11/12/13. `2.7797020033791574e+307` (index 9), pi, 1/3, DBL_MAX,
> DBL_MIN and `1e23` were already optimal and are unchanged — that split is
> the seam between "the uint32 table could hold this depth" and "it could
> not". 9084 -> 9087 assertions (+3), both arches.
>
> One correction to the history the clamp commit left behind: it justified
> clamping at 10 rather than RapidJSON's 9 partly on the claim that AXL's
> integer branch "already indexes `kPow10[9]`". It does not, and the
> bound mattered only because of the FRACTIONAL index (9 is 12.47% of
> conversions). The integer branch's index is post-decrement and tops out
> at **8**; reaching 9 would need `p1 >= 10^9`, and `p1` peaks at
> 798,336,123. That last figure is exhaustive rather than sampled — at a
> fixed exponent `one.e` is fixed and `Mp.f` rises with the significand,
> so `p1` peaks at the largest significand of each exponent, which is 2047
> values to check.
>
> **The review that caught this is worth recording, because my own sweep
> could not have.** I first wrote "never exceeded 7 across 44,000,000
> conversions", which was true of what I measured and false of the code:
> index 8 is reached only by subnormals around 1e-317, roughly 2^-30 of
> the bit space, so a random sweep of any practical size sees index 0 and
> nothing else. A measured maximum is not a bound, and on the one code
> path where the bounds guard was being *removed* I had substituted the
> former for the latter. Both call sites now carry an argument; the
> sweeps only corroborate them. A `_Static_assert` on the table length is
> the tripwire under both.

## Task 5 — migrate a built-in onto the public constructor — DONE

`axl_bufopen` moves to `axl_stream_open_custom`. This is the dogfooding check
that the contract is genuinely sufficient rather than merely plausible — if a
built-in cannot be expressed through it, the API is wrong and better to learn
that now. `axl_fopen` and the text wrapper stay put for diff size; if the
buffer backend migrates cleanly they will too.

**Tests:** the existing buffer-stream tests must pass unchanged. That is the
whole point — behaviour identical, construction path different.

> Landed 2026-08-01. **The contract was sufficient — nothing had to be added,
> widened, or worked around**, and that is the result. `axl_bufopen` now fills
> an `AxlStreamOps` from `AXL_STREAM_OPS_INIT` and calls
> `axl_stream_open_custom(b, &ops, "buffer")`; it no longer names
> `struct AxlStream` at all. Every existing buffer test passed **unchanged**,
> including the two that would have caught a botched translation without
> being written for it (`name: buffer stream`, and `caps: a buffer stream
> reports every capability`, which asserts six of the eight slots survived the
> ops copy). Full detail in `docs/AXL-Stream-Backend-Design.md` §11.
>
> +7 assertions, 9087 -> 9094, both arches. They are the ownership check the
> migration newly makes possible to get wrong: the stream now owns a HEAP COPY
> of the label rather than pointing at a literal, so `axl_fclose` has to free
> it, and nothing above can see the difference. Bucket C, so these are
> protection rather than RED-first — the discrimination was proved by
> sabotage instead, twice, and both were precise: forcing `name_owned = false`
> failed exactly the 4 count/bytes assertions (plus the 2 pre-existing
> `fcloseheap` ones), and dropping `ops.close` failed exactly the same 4 and
> nothing else. The second is the one worth noting: before this test, a
> built-in that leaked its whole context on every close would have gone
> unnoticed by all 557 assertions in the binary.
>
> **Two costs, accepted, neither an expressiveness failure.** The label costs
> one extra ~7-byte allocation per `axl_bufopen` and one more NULL-return
> path, because the contract cannot say "borrow this literal" — and should
> not, since copying is the only safe default for a caller-supplied string.
> And there is no `ctx` getter, so `axl_bufdata`/`axl_bufsteal` still need
> `axl-stream-internal.h`: they are keyed on the STREAM and must recover the
> context from it. `fopencookie` and `funopen` withhold the cookie too, so a
> consumer keys its accessors on its own context handle instead; that is now
> documented in the README rather than left to be discovered. Whether to add
> `axl_stream_ctx()` is left open, not decided — it would cost a `void *`
> nothing can type-check.
>
> One pre-existing hazard surfaced and deliberately NOT fixed here, so it is
> recorded rather than narrowed away: `axl_bufdata` guards only `s == NULL ||
> s->ctx == NULL`, so passing it a *file* stream reinterprets `FileCtx` as
> `BufCtx` and returns garbage. `axl_compress_writer_finish` already shows the
> fix (`s->write != compress_writer_write`); applying it to `axl_bufdata`/
> `axl_bufsteal` is a behaviour change (UB becomes NULL) needing its own
> test-first cycle, and it is not migration fallout — it read the same way
> before.
>
> The other three built-ins (`axl_fopen`, `axl_text_stream_wrap`, the
> compressor) were left alone per the plan's own "for diff size" clause. The
> buffer backend was the right one to try first and the answer it gave is
> "yes", so they are now a mechanical follow-up rather than an open question.
>
> **Done 2026-08-01** — design doc §13. Two of the three were indeed
> mechanical; the text wrapper was not, and the non-mechanical part is the
> finding: its *construction* migrated fine, but it reads its source below
> that source's transcode via the internal `axl_stream_sink_read`, which has
> no public spelling. Reported rather than papered over, and left for its own
> bucket-A cycle because it would be new public API.
>
> **That cycle ran on 2026-08-01 and added no public API** — design doc §14.
> The wrapper requires its source undecoded instead, which makes `axl_read`
> the wire read, so `axl_stream_sink_read` is file-static and
> `axl-stream-internal.h` exports nothing at all.

## Task 6 — housekeeping — DONE

- `.gitignore:20` ignores `tests/.last-pass-count`; there is no `tests/`
  directory. The real path is `test/integration/.last-pass-count`, which is
  tracked deliberately. Stale since the directory rename.
- `include/axl/axl-http-server.h` documents a streamer example as
  `return axl_fread(f, buf, cap, out);` -- wrong argument order, wrong return
  type for the contract it sits in, never sets `*out`, and names a type
  `AxlFile` that does not exist. Found by Task 1; it is the only other
  `axl_fread` example in a public header, so it is exactly what a consumer
  would copy.
- `docs/AXL-Coding-Style.md:668` says 0-1 parameter functions keep the
  signature on one line. Every public header contradicts it, including the
  style doc's own neighbours. Practice is right here (multi-line reads better
  with `///<` param docs); fix the doc.

> Landed 2026-07-31. No behaviour change; nothing to test, so the proof is
> different per item.
>
> The `.gitignore` line matched nothing: the pattern contains a slash, so it
> is root-anchored to a `tests/` that does not exist. `git status --ignored
> --porcelain` and `git ls-files -i -c` before and after are byte-identical
> apart from `.gitignore` itself now showing modified, and `git check-ignore
> -v tests/.last-pass-count` went from a line-20 hit to no match while
> `test/integration/.last-pass-count` stayed unignored and
> `.last-pass-count.tls` kept its (renumbered) rule.
>
> The streamer example was compile-verified both ways in a scratch TU: the
> corrected one builds clean under the library's own flags plus `-Wextra`,
> and the old one still fails with four errors starting at `unknown type name
> 'AxlFile'` -- so the check discriminates rather than just passing. `AxlFile`
> and `axl_file_size` appear nowhere else in the headers; the example was the
> only live use of either name.
>
> The style rule was rewritten to what the headers actually do, split at the
> real seam: zero parameters on one line, one or more multi-line. Measured
> across `include/axl/` -- 551 one-parameter declarations multi-line vs 6 not,
> 89 zero-parameter one-line vs 15 not. The `0-1` phrasing was self-refuting:
> its own worked example, `axl_hash_table_free`, is multi-line in
> `axl-hash-table.h`, and `CLAUDE.md` already said "even single-param".

## Task 7 — DONE

Regression test: `test/integration/test-run-qemu-bg-leak-selftest.sh` (real
guests, host-only, excluded from the parallel matrix because a `df /dev/shm`
delta is a global measurement).

**Two distinct shapes, not one.** The first pass conflated them, and two of its
premises were wrong — both corrected here so the record does not mislead again:

- **`QEMU_PID=` is the `timeout` pid, not QEMU's.** GNU `timeout` fork+execs,
  so the guest is a *grandchild* of the cleanup-owner subshell and
  `pgrep -P "$WRAPPER_PID"` can only ever reach `timeout`. The original
  `qemu still alive: YES / ppid now: 1` observation was `kill -0` on the
  *`timeout`* pid, which does get reparented to init. It was not QEMU.
- **`timeout` does NOT die with the subshell.** Orphans do not die with their
  parent: it survives, fires, and reaps the guest correctly. The
  bounded-lifetime guarantee was intact the whole time.

| | Shape A — SIGTERM the cleanup owner | Shape B — SIGKILL owner + watchdog |
|---|---|---|
| Watchdog | survives, still fires | dead |
| Guest | correctly reaped at timeout | immortal, `ppid 1`, forever |
| State dir | leaks (42,440 KB measured); sweeper *can* collect after 10 min | leaks; sweeper is blind to it |
| Self-protecting | no | **yes** |

Shape B is the one matching the four standalone orphans: the sweeper skips any
directory a live process references (`pgrep -af -- "$dir"`), and the immortal
guest carries the path in argv, so **the leak protects itself**.

**The real defect in shape A** is that `rm -rf "$TMPDIR"` was the last command
of a *killable subshell*. Everything else worked; the `rm` simply never ran.

**`setsid` was the wrong fix** — built and measured, it leaves the leak
byte-for-byte identical, because the `rm` stays in the subshell being killed.
It is also mildly counterproductive: it moves the pair out of the caller's
session, so a session-scoped teardown (`pkill -s`, logind cleanup) can no
longer reach an escaped guest. Do not re-propose it.

**Shipped fix**, two independent parts, one per shape:

1. Give the `rm` to a trap inside the subshell (`EXIT INT TERM HUP`), mirroring
   the foreground branch's existing trap. Bash defers the handler until the
   foreground `timeout` returns, so cleanup still fires *after* the guest exits
   — never an early `rm` under a live guest.
2. `timeout "$TIMEOUT" setpriv --pdeathsig KILL <qemu>`, guarded by a probe
   (`command -v` plus an actual `--pdeathsig` invocation, since an older
   util-linux has the binary but not the flag) with the previous behaviour as
   fallback. `PR_SET_PDEATHSIG` survives the execve into QEMU, so however the
   watchdog dies the kernel takes the guest with it, and shape B stops being
   self-protecting. The death signal is tied to the *watchdog*, which lives the
   guest's whole lifetime, so a normal `--background` guest is unaffected —
   verified explicitly: it outlives `run-qemu.sh`'s exit by 20 s+.

Rejected: teaching the sweeper to reap `ppid 1` guests. A deliberate
`--background` debug session is exactly `ppid 1` and long-lived, so killing one
would be worse than the leak.

## Task 7 — original note

`scripts/run-qemu.sh:738` installs no cleanup trap in `--background` mode,
delegating to a detached subshell that only cleans up after `timeout` returns.
The leading hypothesis for the four standalone orphans of job 2, never
confirmed. Reproduce it or rule it out, and report as a finding either way.
Fix only if confirmed, and then as its own commit.
