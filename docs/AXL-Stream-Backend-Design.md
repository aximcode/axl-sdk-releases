# AXL Stream Backends — opening AxlStream to consumers and tests

**Status:** IMPLEMENTED 2026-07-31. Shape signed off; contract-first review
done and its findings folded in; header contract, implementation and tests
landed together. Two follow-ups were deliberately deferred out of that commit
and have since landed separately: §8b (`axl_fread` does not loop on a short
read) on 2026-07-31, and §11 (the `axl_bufopen` dogfooding migration) on
2026-08-01. §12 added the accessor half (`axl_stream_ctx`) and §13 finished
the migration for the remaining built-ins. §14 closed §13.2's one remaining
gap — a wrapper reading below its source's transcode — **without adding public
API**, by requiring the source undecoded instead; `axl-stream-internal.h` now
exports nothing and has one consumer.
**Date:** 2026-07-31, §11–§14 added 2026-08-01.

`AxlStream` is a closed type: an opaque struct with an internal vtable and a
handful of built-in constructors. Nobody outside `src/stream/` can supply a
stream backend. This closes that, and adds the fault injection the closure has
been standing in for.

---

## 1. The gap, verified

`include/axl/axl-stream.h:40` is `typedef struct AxlStream AxlStream;`, with the
definition private to `src/stream/axl-stream-internal.h`. Public constructors:
`axl_fopen`, `axl_bufopen`, `axl_text_stream_wrap`, `axl_stdin_text`, plus the
compressor wrappers in `axl-compress.h`. There is no way to supply your own.

Two costs, both real:

**Extensibility.** A consumer wanting a stream over a socket, a ring buffer, a
UEFI protocol, or a test double has no path in and must work around the
abstraction. For a library whose selling point is composability that is a hole.

**Testability.** **41** `return -1` sites across `src/stream/*.c`
(23 `axl-stream.c`, 13 `axl-stream-file.c`, 4 `axl-stream-buf.c`,
1 `axl-compress-stream.c`) have no test that reaches them, so the failure
handlers *above* them — the JSON sink, `axl-log-file`, HTTP, 9P — are untested
too. Compare `AxlMem`, which ships `axl_mem_fail_next_alloc` and is used 42
times in the unit suite.

**Scoping honesty:** a custom backend does NOT unlock all 41. Most of the 23 in
`axl-stream.c` are NULL-argument and unsupported-op guards that a backend cannot
reach. The real win is the *backend-error propagation* paths — the ones that
only fire when a sink refuses — plus everything layered above them.

## 2. What the existing layering already settles

Worth stating plainly, because it is the reason this design is small. The four
features that look like they would complicate a custom backend all sit **above**
the vtable already:

| Feature | Where it actually lives |
|---|---|
| Encoding transcode | `axl_read` / `stream_write_now`, around the vtable call |
| Buffering (`NONE`/`LINE`/`FULL`) | `axl_write` chooses before dispatch; `wbuf` is stream-owned |
| Tee | `stream_write_now` |
| `interactive` | consumed by `axl_text_stream_wrap`, never by a backend |
| `eof` / `err` | set by `axl_read`/`axl_write` from the vtable's return (and `err` alone by `axl_pread`/`axl_pwrite`, see below) |

One correction from the contract review: positional I/O sits outside the
transcode and buffering layers entirely, and at the time this was written it
sat outside the flag layer too — `axl_pread`/`axl_pwrite` returned the backend
value raw. That last part was incoherent rather than principled and §8b fixed
it: they now set `err` on a negative return like their sequential twins, while
still deliberately NOT setting `eof` on a zero-length read, because positional
I/O has no stream position for "at the end" to refer to.

A backend therefore supplies **raw byte ops only** and inherits all of it. The
vtable is the correct seam precisely because these live above it.

Two facts the original problem statement had slightly wrong, now confirmed
against the source: the vtable has **8** slots, not 6 (`seek` and `tell` too),
and "optional op" already has an encoding — dispatch NULL-checks every slot
(`s->write == NULL` ⇒ `-1`).

## 3. Prior art, and what each contributes

- **glibc `fopencookie`** — 4 ops in a fixed struct passed **by value**, NULL
  meaning unsupported. Closest analogue; the by-value copy is the move worth
  taking, since AXL already copies ops into its private struct.
- **BSD `funopen`** — 4 ops as separate parameters. Zero ABI surface, but a
  fifth op means a new function name, and AXL has 8 already.
- **GLib `GOutputStream`** — class struct with **reserved padding slots**.
  GObject is a non-starter freestanding; the padding idea is the versioning
  answer.
- **Rust `Read`/`Write`** — short writes explicitly legal at the trait level,
  with `write_all` looping. Contract discipline rather than shape, and directly
  relevant to §5.

## 4. The API

### 4.1 `AxlStreamOps` + `axl_stream_open_custom`

```c
typedef struct {
    uint32_t     struct_size;   ///< sizeof(AxlStreamOps) — see versioning
    uint32_t     version;       ///< AXL_STREAM_OPS_VERSION
    axl_ssize_t (*read)  (void *ctx, void *buf, size_t count);
    axl_ssize_t (*write) (void *ctx, const void *buf, size_t count);
    axl_ssize_t (*pread) (void *ctx, void *buf, size_t count, size_t offset);
    axl_ssize_t (*pwrite)(void *ctx, const void *buf, size_t count, size_t offset);
    int         (*seek)  (void *ctx, int64_t offset, int whence);
    int64_t     (*tell)  (void *ctx);
    int         (*flush) (void *ctx);
    void        (*close) (void *ctx);
} AxlStreamOps;

AxlStream *
axl_stream_open_custom(
    void                *ctx,   ///< backend state, passed to every op
    const AxlStreamOps  *ops,   ///< copied; caller may free after the call
    const char          *name   ///< optional label for diagnostics (NULL OK)
);
```

Signatures are deliberately **identical** to the existing private slots, so the
built-in backends become ordinary consumers of the public contract rather than
a privileged path.

**Zero-init is the idiom.** Unset ops stay NULL, which already means
"unsupported" to the dispatch layer. No new convention.

```c
AxlStreamOps ops = AXL_STREAM_OPS_INIT;
ops.write = my_sink_write;
ops.close = my_sink_close;
AxlStream *s = axl_stream_open_custom(sink, &ops, "my-sink");
```

`AXL_STREAM_OPS_INIT` is **positional**, not designated. GCC suppresses
`-Wmissing-field-initializers` for designated initializers in C only, so the
designated form warns in *every* C++ standard the moment a consumer adds
`-Wextra` — from the macro our own docstring tells them to use, in a project
whose rule is "fix compile warnings before moving on". Verified warning-free
with `-Wall -Wextra -pedantic` across c99/c11/gnu2x and c++17/20/23. Matches
the existing `AXL_QUEUE_INIT` precedent, which is positional for the same
reason. The cost — the macro must be updated when a slot is added — is
enforced by that same warning.

### 4.2 Versioning

`struct_size` first, Vulkan/`getsockopt`-style, and AXL copies
`min(ops->struct_size, sizeof(AxlStreamOps))` bytes. This survives **both**
directions: an old caller against a new library leaves new slots NULL
(unsupported, which is always safe), and a new caller against an old library has
its extra slots ignored rather than read out of bounds. A `struct_size` of 0 or
smaller than the minimum viable prefix is rejected with NULL.

Chosen over GLib-style reserved padding because padding silently accepts a
mismatched struct, while a size field makes the mismatch detectable.

Two refinements from the contract review. **The struct carries a `version`
alongside `struct_size`**, matching `AxlFsProvider`, `AxlFsEntry` and
`AxlCpuException` -- `struct_size` alone carries purely additive growth and
cannot signal a slot whose *meaning* changed, which is exactly what spending
the reserved-negative escape hatch in section 5 would be. And **the minimum
accepted size must be the frozen size of the first published version**, not
a computed floor like "large enough to contain `read`": a computed floor
admits sizes no released AXL ever emitted. Freeze it as a constant with a
`static_assert`, because once v2 ships v1's size can no longer be derived.

Note `struct_size` can never protect the read of itself -- the struct is
unconditionally assumed large enough to hold `struct_size` and `version`.
That is inherent to the pattern, not a defect, and is stated in the header.

### 4.3 Capability queries

Consumers and generic code need to ask before calling, rather than discovering
`-1`:

```c
bool axl_stream_can_read  (const AxlStream *s);
bool axl_stream_can_write (const AxlStream *s);
bool axl_stream_can_seek  (const AxlStream *s);   // axl_fseek only
bool axl_stream_can_tell  (const AxlStream *s);   // independent of can_seek
bool axl_stream_can_pread (const AxlStream *s);
bool axl_stream_can_pwrite(const AxlStream *s);
const char *axl_stream_name(const AxlStream *s);  // "" if unnamed
```

`seek` and `tell` are separate slots with separate NULL checks, so folding them
into one query would itself be the "incidentally true" mistake these exist to
retire -- every built-in happens to set both or neither, but a ring buffer can
report a position it cannot seek to. Likewise `pread`/`pwrite`: a read-only
mapping has one without the other.

These also retire an incidental dependency the JSON work is currently leaning
on: two of its negative tests rely on `axl_stdout` having no read function and
`axl_stdin` no write function. Correct today and correct by design, but nothing
promised it. `axl_stream_can_read` makes the property explicit and assertable.

## 5. The return contract

Stated on `AxlStreamOps` rather than left to inference, because the JSON sink
already distinguishes the cases:

- **`read`/`write`/`pread`/`pwrite`** return bytes transferred. A count **less
  than requested is legal and is not an error** — the caller loops. `0` from
  `read` is EOF. `-1` is a hard error.
- **`seek`/`flush`** return `0` on success, `-1` on error. `tell` returns the
  offset or `-1`.
- **Negative values other than `-1` are RESERVED.** Backends must not return
  them today. Dispatch treats every negative as an error, so reserving costs
  nothing now and leaves room for a `WOULD_BLOCK` signal when a non-blocking
  backend eventually needs one — without which a socket backend would have to
  encode "try again" as a lie.

## 6. Ownership and lifecycle

- `ctx` belongs to the **backend**. `axl_fclose` calls `ops->close(ctx)` exactly
  once, after draining buffered output and freeing `wbuf`; releasing `ctx` is
  that callback's job.
- The `AxlStream` itself belongs to AXL and is freed by `axl_fclose`.
- `ops` is **copied**; the caller may free or reuse the struct immediately.
- `close` may be NULL when there is nothing to release.
- A sink error during the close-time drain cannot be reported (`axl_fclose`
  returns `void`); callers needing that flush explicitly first. Pre-existing,
  restated here so backend authors do not expect otherwise.

## 7. Fault injection — additive, not subsumed

The constructor gives tests a failing stream, but it does **not** replace a
global switch, and the difference decides whether the 41 paths get covered:
`axl_mem_fail_next_alloc` works on code that allocates *internally*, without
restructuring it. A custom stream only helps where the test can construct the
stream. Testing a function that opens its own file still needs a hook.

So both, mirroring the AxlMem spelling exactly:

```c
void axl_stream_fail_next_write(unsigned n);   // Nth write returns -1
void axl_stream_fail_next_read (unsigned n);   // Nth read  returns -1
void axl_stream_fail_next_flush(unsigned n);
void axl_stream_short_next_write(unsigned n, size_t limit);  // Nth write is short
```

Same semantics as the AxlMem original: `n > 0` arms it, earlier operations
succeed, the counter resets after firing. `short_next_write` is what makes the
JSON sink's short-write → "full, dropped" mapping testable at all — the case
that shipped untested and prompted this work.

Injection applies at the **sink boundary** — it counts calls into the
backend's operation, not calls to the public `axl_write`/`axl_read`. That is
the decision the contract review settled, and it is the only one that works:
`stream_write_buffered` returns `count` unconditionally, so an API-level short
write would fabricate a value the buffered path can never legitimately emit,
and an API-level failure would return `-1` without ever reaching the sticky
`err` flag, the flush-prefix retain logic, or the tee — i.e. bypassing exactly
the propagation this exists to test. It also matches `axl_mem_fail_next_alloc`,
which decrements at the point it would call the backend allocator.

Three consequences the header states normatively, because a test author would
otherwise guess wrong: buffering **defers** the tick to the flush; a non-UTF-8
encoding issues one backend write **per code point**, so one `axl_write` can
consume several ticks; and the tee is **excluded**, since a tee that consumed
the tick would let the primary write escape while swallowing the error.

The draft claimed "and `axl_stdout` is UCS-2" as an example of the second.
That is **false** and the pre-commit review caught it: `mStdout` sets no
`.encoding`, so it is an `AXL_ENC_UTF8` stream and its UTF-8 -> UCS-2
conversion happens *inside* `console_write`, i.e. below the sink boundary. A
console write therefore costs exactly one tick however long the string is.
Corrected in the header and README, and pinned by a test — the sentence was
the sort of plausible-and-wrong that leaves an armed counter to ambush an
unrelated later assertion.

As landed: all four `s->write(` sites and both `s->read(` sites in
`axl-stream.c` funnel through one `sink_write` / `sink_read` pair, so the
counter's meaning cannot vary with encoding and a new call site cannot quietly
escape. `sink_write` carries an `inject` flag that is `false` at exactly one
caller — the tee — which makes the exclusion greppable rather than implied by
which helper someone happened to call. `sink_flush` is the same idea for
`axl_fflush`'s backend call. Nothing in those three helpers logs: `axl_debug`
would re-enter `axl_write` and recurse straight back in.

Injection works on **every** stream including the built-ins, and is
release-safe (one predicted branch on a path that already dereferences a
function pointer, matching how AxlMem justifies its own). It is not
reentrancy-safe: a callback writing at raised TPL can steal an armed tick.

## 8. Testing

Bucket A: header + docstrings first, contract-first review of the header alone,
then failing tests, confirm RED, implement, refactor while green, pre-commit
review. Both arches. The test backend built on `axl_stream_open_custom` becomes
the vehicle for reaching the backend-error paths of §1.

## 8b. Required follow-up this uncovered — DONE

`axl_fread` issues a **single** `axl_read` and returns `n / size` — it does not
loop. Against the old closed set of backends that was invisible, because none
of them short-read. The moment a consumer supplies a socket backend, a
legitimate short read makes `axl_fread` return a short item count that its
caller cannot distinguish from EOF. The header carries a caution today; the
real fix is to make `axl_fread` loop until the item count is satisfied or the
stream ends, which is what `fread` has always meant. Its own docstring already
says "Like fread()".

Scoped as a separate change with its own regression test rather than folded in
here, because it alters shipped behaviour on a path every existing caller uses.

**Outcome.** Both helpers now loop. Termination is the design point: `fread`
stops at the item count, at a 0-length read (end of input), or on -1;
`fwrite` stops at the count, on -1, or at the FIRST 0. That last rule is the
one worth stating — a backend 0 is a legal "accepted nothing, try again", but
retrying it inside the loop would spin, since nothing can drain the sink while
the loop holds the CPU. So a short `fwrite` count with `axl_ferror()` false is
the "retry at your own cadence" signal, and with it true is a real failure.
`size * count` overflow is refused rather than serviced at a wrapped size.

Two coherence fixes rode along, both found while writing that contract down:
`axl_pread`/`axl_pwrite` now set `err` on a negative return (they set nothing
before, unlike their sequential twins) while deliberately NOT setting `eof` on
a 0-length read — positional I/O has no stream position; and `axl_fclose` no
longer hands a `.data` address to `axl_free` when passed one of the five
static console streams. That last one wedged the firmware outright in the
red-test run, which is what a consumer would have hit.

`tools/tr.c` moved from `axl_fread` to `axl_read` in the same change: a filter
wants "translate what arrived", and a looping `fread` on an interactive
console blocks until 4 KiB has been typed. GNU `tr` reads raw for the same
reason.

## 9. Deliberately not in scope

- Async / non-blocking streams. §5 reserves the return space; nothing more.
- Seek/tell semantics for non-seekable backends beyond "return `-1`".
- ~~Rewriting the built-in backends onto the public constructor.~~ Deferred
  out of the landing commit, then done for the buffer backend (§11) and
  finally for **all** of them (§13): `axl_fopen`, `axl_text_stream_wrap` and
  `axl_compress_writer` now go through `axl_stream_open_custom` too. Nothing
  fills `struct AxlStream` directly any more except the five static console
  streams, which are objects in `.data` and cannot come out of a constructor.

## 10. Sign-off outcomes

1. **`struct_size` vs reserved slots** — size field, plus a `version`
   (§4.2). Landed. The minimum is the frozen v1 size, a literal `72u` pinned
   by a `_Static_assert`.
2. **`name`** — kept. A `name_owned` flag decides whether `axl_fclose` frees
   the label, which is what stops it handing `.rodata` to `axl_free`. The
   split is by CONSTRUCTION PATH, not by built-in-ness: a constructor that
   fills the struct directly points at a literal, and anything going through
   `axl_stream_open_custom` owns a heap copy. Since §13 the only literals
   left are the five static console streams — every *constructor* now owns
   its label.
3. **Should the built-ins migrate now or later?** Later, per §9 — and they
   have all now gone: `axl_bufopen` first (§11), then `axl_fopen`, the text
   wrapper and the compressing writer (§13). The contract was sufficient
   every time.
4. **`short_next_write`** — kept, and it is the hook that reaches
   `stream_flush_prefix`'s retain-the-tail branch, which nothing else could.

## 11. The dogfooding check — `axl_bufopen` on the public constructor

Landed as its own change. `axl_bufopen` no longer touches `struct AxlStream`:
it fills an `AxlStreamOps` from `AXL_STREAM_OPS_INIT` and calls
`axl_stream_open_custom(b, &ops, "buffer")`. **The contract was sufficient**
— nothing had to be added, widened or worked around, and no existing buffer
test changed. Recorded in detail because "we could not express X" was the
outcome worth catching, and the near-misses are worth knowing even though
none of them bit:

- **Every field the private constructor set has a public route.** Seven ops
  plus `ctx` plus `name`; `flush` stays NULL straight from the macro. The
  other stream fields `axl_bufopen` never set (`encoding`, `interactive`,
  `buf_mode`, `tee`, `eof`/`err`) are §2's above-the-vtable layer and have
  public setters where they need one — so their absence from `AxlStreamOps`
  is the design working, not a gap.
- **Nothing needed the struct writable after construction.** `axl_bufsteal`
  mutates the *context*, not the stream.
- **The failure path already matched.** `axl_bufopen` frees its context when
  construction fails, which is exactly the "a refused open never calls
  `close`, `ctx` is still yours" clause §6 publishes.

Two costs, both accepted, neither an expressiveness failure:

1. **`name` is now heap-copied, not a literal** — one extra ~7-byte
   allocation per `axl_bufopen`, and one more way for it to return NULL.
   The contract cannot say "this label is static, borrow it", and it
   should not: copying is the only safe default for a caller-supplied
   string. Observable only in allocation *counts*, so `axl_stream_name`
   still answers `"buffer"` and §10.2's claim that only the custom path
   heap-copies is now false — corrected there and in the two source
   comments that repeated it.
2. **There is no `ctx` getter, so the *accessor* half does not migrate.**
   `axl_bufdata`/`axl_bufsteal` are keyed on the stream and must recover
   the context from it, which keeps `axl-stream-buf.c` on
   `axl-stream-internal.h`. `axl_compress_writer_finish` is the same shape.
   A consumer cannot build a stream-keyed accessor and must key on its own
   context handle instead — the same restriction `fopencookie` and
   `funopen` impose, and documented in the README rather than left to be
   discovered. Adding `axl_stream_ctx()` would lift it, at the price of a
   `void *` nothing can type-check; not taken here, and noted as an open
   question rather than a decided "no".

   > **Settled in §12.** The open question is closed, and the price turned
   > out to be avoidable: the getter takes the caller's `AxlStreamOps` as
   > well as the stream, so it type-checks after all. `axl-stream-buf.c`
   > no longer includes `axl-stream-internal.h`.

### 11.1 The type-confusion hazard — FIXED

The migration surfaced one pre-existing hazard without changing it:
`axl_bufdata` guarded only `s == NULL || s->ctx == NULL`, so handing it a
*file* stream reinterpreted `FileCtx` as `BufCtx`. It was recorded here rather
than fixed, because it is a behaviour change needing its own test-first cycle.
That cycle has now run, and both accessors gained the vtable-slot check
`axl_compress_writer_finish` already used:

```c
static bool
is_buf_stream(const AxlStream *s)
{
    return s != NULL && s->read == buf_read && s->ctx != NULL;
}
```

Writing the regression test showed the hazard was **worse than "returns
garbage"**, in two ways this section had not accounted for:

- `axl_bufsteal` is destructive, not just wrong. `BufCtx` is 32 bytes and
  `FileCtx` is 16, so zeroing `alloc` and `read_pos` wrote 16 bytes **past the
  end of the file context's allocation**. It also handed the caller the live
  firmware file handle as a pointer the contract says to `axl_free()`.
- The damage is not confined to the call. The RED run's `AxlTestIO` binary
  stopped producing output right after the corrupting `axl_bufsteal` — the
  refused stream was not merely useless afterwards, it took the image with it.
  A test written only around the return value would have missed that; the
  assertion that caught it is "the refused stream is undamaged and still
  writable".

`read` is the discriminating slot because `buf_read` is file-static: no
consumer can name it in an `AxlStreamOps`, and no other backend holds it. A
custom stream can present the same vtable *shape* — read + write + seek + tell
+ pread + pwrite — but not the same function *pointers*, so shape alone never
passes. The check is therefore not spoofable from outside the library, and
inside it only `axl-stream-buf.c` can take the address at all.

Two consequences worth stating, since they are the behaviour change:

- A **text wrapper over a buffer stream** is refused. Its `ctx` is the inner
  `AxlStream *`, not the `BufCtx`, so it was never safe; it now says so.
- The refusal is **inert**. `size` is left untouched (matching the NULL-stream
  path), and the refused stream is not modified at all.

## 12. The accessor half — `axl_stream_ctx`

Landed 2026-08-01, closing §11's one open question. `axl_bufdata` and
`axl_bufsteal` now reach their context through the **public** getter, and
`src/stream/axl-stream-buf.c` includes no private header at all. That is the
same dogfooding proof §11 gave for the constructor half, and it is a stronger
one: §11 showed a built-in could be *built* through the contract, §12 shows it
can be built and *served* through it, so nothing about `axl_bufopen` is
privileged any more.

### 12.1 The shape, and why it is not a bare getter

```c
void *axl_stream_ctx(const AxlStream *s, const AxlStreamOps *ops);
```

The second argument is the whole design. A bare `axl_stream_ctx(s)` was
rejected, and not on general principle — §11.1 is the specific reason.
`axl_bufdata` shipped guarded only by `s->ctx != NULL`, so a file stream's
16-byte `FileCtx` was read as a 32-byte `BufCtx` and `axl_bufsteal` then zeroed
16 bytes past the end of the allocation. It did not merely return garbage; the
test image stopped producing output. Handing consumers an unchecked getter
would have re-opened that for every backend at once, and unlike the in-tree
case there would be no `is_buf_stream` to add afterwards, because a consumer
cannot see the struct.

So the caller proves what it expects by naming its own operations, and the
context comes back only on a match. Three properties follow, and each is
asserted:

| property | why it holds |
|---|---|
| Shape is never enough | Slots hold *addresses*. A stream with read+write+seek+tell is refused unless they are the same four functions. |
| Built-ins are unreachable | Every built-in backend's ops are file-static. No consumer can name one, so no consumer-built `ops` can match. This falls out of the design; it is not a rule that had to be written and could be forgotten. |
| Two backends cannot collide | Different authors write different functions. The only way to match is to *be* that backend. |

The third has one honest caveat: a consumer that deliberately reuses one
`AxlStreamOps` across several context **types** gets its own contexts back
undifferentiated. That is correct — the ops identify the constructor, and a
constructor that produces two shapes has to discriminate inside its own
context. The getter refuses foreign streams, not a backend's confusion about
its own.

### 12.2 Why diverge from `fopencookie` and `funopen`

Both withhold the cookie entirely, and it is worth being explicit that this is
not a case of knowing better than glibc. Neither API copies a versioned
operations struct — `fopencookie` takes four bare function pointers by value,
`funopen` four arguments — so neither has any token a caller could present to
prove which backend it means. Withholding was their only safe option, and the
cost is visible in every consumer of them: a parallel handle carried alongside
the `FILE *`. AXL already stores a normalised copy of the ops at open time, so
the token exists for free. The divergence is therefore "we can check, so we
offer it checked", not "we think the restriction was wrong".

The nearer prior art is object systems rather than stdio. GObject's
`G_TYPE_CHECK_INSTANCE_CAST` and Rust's `Any::downcast_ref::<T>()` are the
same move — recover a typed pointer only on a proven runtime match, NULL/None
otherwise — and APR's `apr_pool_userdata_get(&data, key, pool)` is the same
again with a string key. Taking the ops struct as the type token is the C
spelling of that idea using a token this library already had.

### 12.3 What the tests pin, and the one thing they cannot

13 assertions were RED against a deliberately-planted naive
`return s->ctx` stub, and every one of them is a *safety* property rather than
a functional one — the naive getter passes "it returns the context" and fails
"it refuses the wrong thing", which is exactly the discrimination worth having.
Sabotages:

| sabotage | failures |
|---|---|
| compare only the `read` slot (the weaker single-slot design) | 3 — differing slot, missing slot, extra slot |
| desync the buffer accessor's ops from the constructor's by one slot | 35 — every buffer, printf, encoding and buffering test |
| `stream_ops_normalize` stops zeroing its copy | **0** |

The 35 is the shared `buf_ops()` helper earning its place: with the two sides
free to drift, `axl_bufdata` silently stops recognising its own streams and
most of the file goes with it.

**The 0 is a finding, not a pass.** The `axl_memset` in
`stream_ops_normalize` is unreachable today: v1's frozen size *equals*
`sizeof(AxlStreamOps)` (pinned by a `_Static_assert`), so the floor check has
already refused everything a partial `memcpy` could leave half-written. It is
kept rather than deleted because it becomes load-bearing the moment a v2 grows
the struct — without it the new slots would be compared against, and in
`axl_stream_open_custom` *installed from*, uninitialised stack. The
`_Static_assert` is the tripwire that forces a re-read then. Recorded here
rather than left as a coverage claim nobody checked.

One other test does not discriminate on its own and is worth naming:
`axl_stream_ctx(axl_stdout, &ops)` passed even against the naive stub, because
the static console streams carry a NULL context. It is kept as a
regression pin, not counted as proof.

### 12.4 Not migrated

`axl_compress_writer_finish` has the same shape but cannot follow yet:
`axl_compress_writer` builds its stream with `axl_stream_new()` and fills the
struct directly rather than going through `axl_stream_open_custom`. Migrating
the *constructor* first is the prerequisite, exactly as it was for the buffer
backend, and that is the §11 "mechanical follow-up" for the remaining
built-ins. Left alone here to keep this change to one idea.

> **Done in §13.** The constructor migrated, and `axl_compress_writer_finish`
> is now built on `axl_stream_ctx` — `axl-compress-stream.c` includes no
> private header at all.

### 12.5 What the contract-first review changed

The header went to an independent review before any implementation existed,
per the bucket-A workflow. It came back with the shape intact and the
*justification* wrong in three places, which is the review paying for itself:

- **"Every slot holds the address of a function the backend author wrote"
  was false for the common case.** The docstring's own worked example leaves
  six of eight slots NULL. The mechanism is fine — NULL slots discriminate
  too — but the argument leaned on inflated entropy. Rewritten to the true
  claim: one file-static function is enough, and it is the *set*, NULL
  entries included, that is compared.
- **The recommended idiom was the drift-prone one.** The first draft told
  authors to rebuild the ops inside each accessor and demoted the
  build-it-once form to a performance footnote. That is backwards: the two
  copies drift, the match then fails forever, and the accessor answers a
  plausible default with no diagnostic. The library's own
  `axl-stream-buf.c` had already used the safe form, so the docstring was
  recommending against what the implementation did. Now one shared
  definition is the idiom, and it is **executed** as a test
  (`test_stream_ctx_docstring_idiom`) so the advice cannot rot away from
  the behaviour — the same treatment the UTF-8 encode idiom got.
- **"Two different backends cannot present the same set" was an absolute
  with counterexamples.** One ops block serving several context *types* is
  the one that will actually happen, and the obligation it implies — one
  `AxlStreamOps` per context type — was assumed and never stated. Also
  restated: this is a type-confusion guard, not access control, since any
  code that can name the operations can assemble a matching block. The
  reviewer additionally tested the identical-code-folding hypothesis rather
  than asserting it (GCC did not fold address-taken statics at `-O0`,
  `-Os` or `-O2`, and GNU ld has no ICF), which is why the text now says
  what holds rather than "cannot".

Two shape changes came out of it as well. `s` is `const AxlStream *`,
matching every neighbouring query (`axl_stream_name`, the four capability
queries) — `axl_bufdata` is non-const only because its twin `axl_bufsteal`
mutates, and this call has no such twin. And the neither-read-nor-write rule
moved *into* `stream_ops_normalize`, so both public entry points inherit it
from one place: without that, an empty `AXL_STREAM_OPS_INIT` was a legal
probe, refused only because no stream happens to have both slots NULL.

Two suggestions were declined, with reasons:

- **A warning on a malformed `ops`.** It would be asymmetric with
  `axl_stream_open_custom`, which returns NULL silently, and the case is
  close to unreachable in practice: a caller whose ops block is malformed
  never got a stream in the first place, so the only way to reach the query
  with a bad block is to have built a *second* one — which is the drift
  problem, whose answer is the one-definition rule the docstring now leads
  with.
- **A `bool` + out-param form.** Of the three failure modes, two are
  deliberately conflated already (not-mine, NULL context) and the third is
  a programming error; the pointer return keeps the call site one
  expression, which is the whole ergonomic point.

### 12.6 Honest limits of the test set

Two assertions do not discriminate under any single sabotage and are kept as
last-line guards rather than counted as proof:

- The **empty-probe** pair (`an ops with neither read nor write refused`,
  `a close-only ops is still refused`) survives removing the
  neither-read-nor-write rule *and* survives making the comparison ignore
  NULL slots. Only the two sabotages **together** reach them. That is
  layered defence working as intended, but it means neither guard alone is
  what those assertions measure.
- `axl_stream_ctx(axl_stdout, &ops)` passed even against the naive
  unchecked stub, because the static console streams carry a NULL context.

## 13. The remaining built-ins — file, text wrapper, compressing writer

§11 migrated `axl_bufopen` as the dogfooding proof and §9 left the rest out
purely for diff size. They are done now. **Nothing fills `struct AxlStream`
directly any more except the five static console streams**, and those cannot:
they are objects in `.data`, not allocations, so no constructor can produce
them (`axl_fclose`'s `on_heap` guard is what keeps them out of `axl_free`, and
this change goes nowhere near it).

Per site, and the honest answer for each:

| Site | Contract sufficient? | Private header after |
|---|---|---|
| `axl_fopen` (`axl-stream-file.c`) | yes, unchanged | **none** |
| `axl_compress_writer` + `_finish` (`axl-compress-stream.c`) | yes, and `axl_stream_ctx` closed the accessor half | **none** |
| `axl_text_stream_wrap` / `axl_stdin_text` (`axl-stream-text.c`) | for CONSTRUCTION yes; one read-path call was not public | **none**, as of §14 |

`src/fs/axl-fs.c` also dropped its include of `axl-stream-internal.h`, which
was stale — it uses `axl_fopen` and nothing else from the private layout.
`src/stream/axl-stream.c` keeps it, correctly: it *is* the implementation.

### 13.1 What each site needed, and what nearly bit

**`axl_fopen` — the plain case.** Eight ops, one context, one label. The
failure path is the only thing worth recording: the old code released the
context by hand (`axl_backend_file_close(&handle); axl_free(f)`), and the new
one calls `file_close(f)` instead, because that callback *is* exactly that
release and nothing more. Delegating is DRY and cannot drift. **The
compressing writer proves the rule is not general** — see below.

**`axl_compress_writer` — where delegating the cleanup would be a bug.**
`compress_writer_close` finalizes on the way out: it compresses the
accumulator and writes the result to the sink. Calling it from a *refused*
open would emit an empty gzip member into the caller's sink for a stream that
never existed. So this one spells its cleanup out. The rule, stated once:
**delegate to your `close` only if `close` is pure release.**

`axl_compress_writer_finish` migrated with it and is the second instance of
§12's accessor shape — a shared `compress_writer_ops()` helper feeds both the
constructor and the getter, so they cannot desync. Two small behaviour
refinements fall out, both strictly narrowing and both intended:

- the old check was `s->write != compress_writer_write`, one slot; the new one
  compares all eight. Nothing else in the tree can present that write slot, so
  no reachable input changes answer.
- the explicit `s == NULL` guard is gone, now riding on `axl_stream_ctx`
  refusing a NULL stream. That is a promise one layer down, so it got its own
  assertion rather than trust.

**`axl_text_stream_wrap` — construction migrates, one read does not.** Two
private uses had to be looked at separately, and they came out differently:

- `src->read == NULL` (refuse to wrap a write-only source) is just
  `axl_stream_can_read(src)`, which is public and NULL-safe. Gone.
- `axl_stream_sink_read(src, ...)`, used by both the classify-time probe and
  every forwarded read, is **not** expressible. See §13.2.

The `interactive` question raised against this site turned out to be a
non-issue: the wrapper reads it with `axl_stream_get_interactive(src)` and
sets it with `axl_stream_set_interactive(s, true)`, both public and both
already in the code. Same for `axl_stream_set_encoding` on the classify
result. Everything the wrapper does *after* construction was already above the
vtable, which is §2's layering working.

### 13.2 The one gap — a wrapper cannot read wire-side

`axl_text_stream_wrap` reads its source **below that source's own transcode**,
because choosing the encoding is the wrapper's entire job: `axl_read` would
apply `src`'s encoding first and the wrapper would then transcode the result a
second time. The internal `axl_stream_sink_read` is how it reaches past that,
and the public surface has no spelling for it.

This is a **wrapper-composition** gap, not a custom-backend one, and the
distinction is the point: the stream this file *builds* goes through
`axl_stream_open_custom` exactly like a consumer's, and §12's `axl_stream_ctx`
already documents that a wrapper is deliberately not reachable *through*. What
is missing is the other direction — reading the wrapped stream raw.

Deliberately **not** fixed here, for two reasons:

1. It is new public API, so it belongs in a bucket-A cycle of its own
   (contract-first review, red-first tests), not appended to a migration whose
   whole claim is that behaviour is identical.
2. It is not obviously the right API. A public "read someone else's stream
   below its transcode" is a layering violation offered as a convenience, and
   the honest advice for a consumer — *read with `axl_read`, and set the
   encoding on your wrapper rather than on the source* — has no downside
   whenever the source is at the default `AXL_ENC_UTF8`, which is every source
   a wrapper is likely to be handed. The in-tree wrapper is stricter than that
   only because it must not assume it.

Recorded in `src/stream/README.md` under *Wrapping another stream* so a
consumer meets it as advice rather than as a missing symbol, and in a comment
on the include itself so the next reader knows why one private header survived.

> **Closed in §14, and the second reason above is why it closed the way it
> did.** The honest advice turned out to be sufficient *and* enforceable, so
> the wrapper adopted it instead of the library growing a call to work around
> it. No new public API.

### 13.3 Cost, and what the tests pin

The same cost §11 accepted, now paid three more times: `"file"`, `"text"` and
`"compress"` are heap copies rather than literals, so each constructor carries
one extra small allocation and one more NULL-return path. Invisible to
`axl_stream_name`, which is exactly why it needs assertions of its own.

+10 assertions (9190 → 9200), bucket C — protection for what the migration
newly makes possible to get wrong, not red-first. Five sabotages, all
discriminating:

| Sabotage | Failed |
|---|---|
| drop `ops.close` from `file_ops()` | the 2 `migown` file count/bytes assertions, nothing else |
| drop `ops.close` from `text_stream_ops()` | the 2 `migown` text count/bytes assertions, nothing else |
| drop `ops.close` from `compress_writer_ops()` | the 2 `compown` assertions **+** the pre-existing `fclose finalizes implicitly` |
| drop `ops.flush` from `file_ops()` | 2 pre-existing flush-fail assertions — the slot was already pinned |
| change all three labels | the 3 pre-existing `name:` assertions |

The last two are the useful ones: they say the *existing* suite would have
caught a botched translation of the flush slot and of the labels without
anyone writing a test for this change. What it could not have caught is a
leak, which is what the new assertions add.

## 14. The wrapper gap — closed by subtraction

**Landed 2026-08-01.** §13.2 left one thing not fully public: a wrapper could
not read the stream it wraps below that stream's own transcode, which is what
`axl-stream-text.c` used the internal `axl_stream_sink_read` for. It was
scoped as a bucket-A cycle for new public API. **No public API was added.**
`axl_stream_sink_read` is now file-static in `axl-stream.c` and
`axl-stream-internal.h` exports no functions at all — it is the struct
definition and nothing else, with exactly one consumer, the implementation
that owns it.

### 14.1 Why not a public wire-side read

Three shapes were considered. Naming them matters, because "add
`axl_stream_read_wire`" is the obvious move and it is wrong.

| Shape | Verdict |
|---|---|
| `axl_stream_read_wire(s, buf, n)` — public read below any stream's decode | **Rejected.** Publishes the layering violation it exists to contain. Its only correct use is "I am a wrapper and I own the decode"; anywhere else it silently skips a decode the stream was configured to perform. And it cannot be made safe by construction — using it correctly is a convention, which is what we were trying to stop relying on. |
| `axl_stream_open_wrapper(src, ctx, ops, name)` — a first-class wrapper constructor | **Rejected, and not for the obvious reason.** It is attractive: it would let `axl_stream_set_encoding` refuse at the *point of the mistake* rather than N reads later, and delete the lifetime guard below. But it needs a back-reference the wrapper decrements on close, and the contract permits `axl_fclose(src)` before `axl_fclose(wrapper)` — the wrapper deliberately owns nothing — so the decrement is a use-after-free on a legal ordering. That kills it. |
| Require the source **undecoded** and read it with the public `axl_read` | **Shipped.** |

The third works because of a fact §2 already established and this change
finally leans on: at `AXL_ENC_UTF8` the encoding layer is a passthrough and
`axl_read` dispatches straight to the backend. The public read **is** the wire
read; there was never a missing capability, only a missing precondition.

### 14.2 The rule, stated transport-neutrally

> A filter that moves opaque bytes through another stream requires that stream
> to be at `AXL_ENC_UTF8` — not because the bytes are text, but because any
> other setting means that stream is transforming them, and a filter's bytes
> are not characters.

That one sentence covers all three in-tree filters, which is the point: the
first draft of this change stated it as a text-specific "one decoder per byte
stream" and enforced it in one place. The contract review's sharpest finding
was that the *library breaks its own new rule twice*, both live bugs:

- **`axl_compress_reader`** already read its source with `axl_read` and did
  not require `AXL_ENC_UTF8`. A gzip source someone had set to `UCS2_LE` gets
  transcoded before `axl_decompress` sees it — NULL, with a decode error that
  says nothing about the cause.
- **`axl_compress_writer`** writes the finished member to its sink with
  `axl_write`. A sink at `UCS2_LE` runs `write_transcode` over binary DEFLATE
  and produces a corrupt archive **with `AXL_OK` returned**.

Both now refuse, and the writer re-checks at `finish` because that is when the
sink is actually written. Enforcing in one of three would have been the worst
available outcome: it teaches consumers a rule the library visibly does not
follow.

### 14.3 What the refusal costs, and the two shapes it takes

`axl_text_stream_wrap` over a source that already decodes now returns NULL
where it used to work. That composition is self-contradictory — you told the
stream what its bytes mean, then asked a sniffer to work it out — and
`tools/cat.c` already treats the two as mutually exclusive: given `-e ENC` it
sets the encoding and does **not** wrap; without it, it wraps and does not
set. The in-tree flagship consumer was already obeying the precondition.

Two refinements the review forced, both of which changed behaviour from the
first draft:

- **A text wrapper is accepted as a source** whatever encoding its own
  classifier settled on, recognised through the public `axl_stream_ctx`. Its
  output is UTF-8 by construction, so reading it decodes exactly once. Without
  the exemption, whether a text stream could be wrapped again would depend on
  the bytes in the file underneath it — ASCII input leaves the inner wrapper
  at `AXL_ENC_UTF8`, UTF-16 input does not. That is a composability regression
  a generic `read_as_text(s)` helper would hit at random. Over such a source
  the outer wrapper is a **passthrough that does not classify** — that second
  half is not optional, and §14.7 is what happens when it is left out.
- **The check runs before the allocation and before any read**, so a refusal
  leaves the source at the position and encoding the caller handed over. That
  is what distinguishes "refused" from "drained it and failed", and it is the
  assertion the compress-reader test needed — a NULL return alone does not
  discriminate, because the *unfixed* path returns NULL too.

### 14.4 The lifetime clause, and what a per-read guard can honestly promise

A caller can reach around a live wrapper and set an encoding on its source, so
the check is repeated on every read. The first draft of the docstring promised
"every subsequent read fails with -1 and `axl_ferror()` true". The review
showed that promise was undeliverable in three ways, all of which are fixed:

1. **Probe pushback.** Up to `PROBE_SIZE` (64) bytes can be held by the
   classifier. A guard placed after the drain would serve them first — up to
   three successful reads — and the partial-success convention
   (`return emitted ? emitted : got`) would then report the failure as a
   *short read* with no error flag at all. **Fix:** the guard is the first
   statement in `text_stream_read`, above the drain, and returns -1
   unconditionally.
2. **The wrapper's own `in_pending`.** On a BOM path the wrapper is at
   `UCS2_LE`, so `read_transcode` drains up to 3 decoded bytes before reaching
   `pull_wire`. **Fix:** the docstring says so — `axl_ferror()` becomes true
   and reads return -1 *once the wrapper's own decoded leftovers have
   drained*.
3. **`axl_read(s, buf, 0)`** returns 0 before dispatch. A universal
   quantifier with a counterexample; the wording no longer uses one.

Two properties came out of getting this right and are now published: the check
is **live, not latched** — restoring the source to `AXL_ENC_UTF8` revives the
wrapper with its probe bytes intact, because the guard returns before
consuming any — and the -1 is **not distinguishable** from an I/O error, so
callers must not branch on it. `-1` is the only defined failure value (§5),
and spending it on a programming error is the price of not inventing a
negative code that every `< 0` caller would misread.

### 14.5 Stating the rule where the mistake is made

`axl_stream_set_encoding` now carries it. That is the review's "report the
error where it is made" finding: a consumer reading the *setter's* docstring
previously had no path to discovering that the call poisons a live wrapper or
corrupts an attached compressor, returns `AXL_OK` regardless, and surfaces
later in a different component as an unexplained stream failure. The mirror
footgun is documented too — setting an encoding on a *wrapper* overwrites the
verdict its classifier reached, which is the one thing that wrapper exists to
produce. `AxlStreamOps` carries a short form for filter authors, who are the
people who need it.

One consequence worth recording rather than fixing here: `axl_stdin` is a
process global and `axl_stream_init()` resets nothing on the five static
console streams — not `encoding`, not `eof`, not `err`. So an encoding left on
`axl_stdin` by a tool that died before restoring it makes every later
`axl_stdin_text()` return NULL for the rest of the image's life, which in a
resident driver means every later dispatch. Under the old behaviour the leak
was invisible because the wrapper wire-read anyway. Documented on
`axl_stdin_text` and logged at debug level on the refusal path;
**making `axl_stream_init()` reset the statics is a follow-up**, not folded in
here because it is a lifecycle change with its own blast radius.

> **Follow-up landed** (§14.8).

### 14.8 `axl_stream_init` resets what it publishes

`axl_stream_init()` now puts each of the five back to the ground state before
handing the globals out, through the **same** `stream_reset_to_ground` helper
`axl_fclose` uses — two hand-maintained copies of "what ground state means"
for the same structs would have been a second bug in waiting.

**Reachability, checked before shipping.** There are exactly three in-tree
callers, and they are mutually exclusive once-per-image entry paths:
`_AxlEntry` in `src/crt0/axl-crt0-minimal.c`, `_axl_init` in
`src/runtime/axl-runtime.c` (the native CRT0's app path), and
`axl_driver_init` in `src/util/axl-driver.c`, which every driver macro funnels
through — `AXL_DRIVER`, `AXL_SHARED_DRIVER` and `AXL_SERVICE_DRIVER` alike.
Drivers supply their own entry point and never link a CRT0, so no image
reaches two of them. Nothing in the shared-driver dispatch path re-inits
either: the stdio bridge swaps the *backend handles* underneath the statics
(`axl_backend_shell_stdin` consults a live bridge), leaving the `AxlStream`
structs untouched. So on every path that exists today the reset is a no-op:
the statics are already at ground when the first and only call runs.

**What it therefore does and does not fix.** It does not, on its own, clear
state between a resident driver's dispatches — nothing calls it per dispatch,
which is now stated where a driver author will read it. What it fixes is that
the ground state was *incidental* (the statics happened to start zeroed)
rather than *established*, so there was no supported way back after a tool
configured a standard stream and did not restore it. `axl_stream_init()` is
that way back now, and the README's long-standing "resets it to how
`axl_stream_init` left it" is finally a definition rather than a coincidence.

**Why discarding is safe.** The one hazard in resetting a live stream is
losing bytes, and the shared helper drains before it frees: pending buffered
output goes to the sink, exactly as `axl_fclose` on a static already did. What
is discarded is configuration — tee, buffering, encoding, interactive mark —
which is what "initialize the standard streams" asks for, and is identical to
what `axl_fclose(axl_stdout)` (documented as safe for generic code to call)
has always discarded. So the surprise surface is one consumers are already
exposed to.

+11 assertions (9237 → 9248). The discriminating one is the end-to-end failure
mode rather than a field read: leak `AXL_ENC_UCS2_LE` onto `axl_stdin`,
confirm `axl_stdin_text()` returns NULL, re-init, and require the wrapper to
open again. Three sabotages, all discriminating: drop the reset calls from
`axl_stream_init` (7 failures — the original defect), have the reset discard
rather than drain (8, spanning both callers of the shared helper, which is the
point of sharing it), and have it leave `encoding` alone (2 — the headline
failure and nothing else).

One test-only hazard worth recording: the GREEN assertion marks `axl_stdin`
interactive before wrapping it. That is a harness guard, not part of the
contract — classification eager-reads a non-interactive source, and under the
test harness `axl_stdin` may be the console pseudo-file, where a blocking read
would wedge the binary and every later one in the same boot. The refusal being
tested is checked *above* the interactive short-circuit and applies uniformly,
so suppressing the probe costs the assertion nothing.

### 14.6 Cost, and what the tests pin

+35 assertions (9200 → 9235). **15 were red-first** — 14 against the
unchanged implementation, plus one against the first draft of *this* change
(§14.7). The remaining 20 are controls, fixtures and idiom guards that passed
at RED by construction; named as such rather than counted as proof. Two earn
their place beyond documentation: the executed lend-and-restore idiom, which
exists so the docstring's advice cannot rot away from the behaviour, and the
leftover-drain assertions, which are the only thing standing behind the
contract's most specific promise (§14.4's point 2) — the mechanism for that
one lives *above* the wrapper, in `read_transcode`'s `in_pending`, where the
wrapper's own guard cannot see it.

Nine sabotages, all discriminating, one per guard: drop the per-read check
(3 failures), move it BELOW the pushback drain (2), have it return `0` rather
than `-1` (4), drop the construction refusal (4), drop the wrapper exemption
from `source_is_undecoded` (2), re-enable classification over a wrapper (1),
drop the compress-writer construction guard (1), drop its finalize re-check
(2), drop the compress-reader guard (1).

Three assertions were rewritten during the RED phase for passing for the
*wrong* reason, which is the failure mode the exact-string discipline exists
to catch. The compress-reader refusal was first fed junk, so its NULL came
from the ordinary decode error; and even with a valid gzip member the NULL is
still ambiguous, because the **unfixed path also returns NULL** — what
discriminates is the source's *position*, since a refusal never reads it. The
last sabotage confirms exactly that: dropping the reader's guard fails only
the position assertion, never the NULL one.

### 14.7 What the pre-commit review changed

The header went to an independent review before implementation and the diff
went to another before commit. The second one found a **real bug in the first
one's fix**, which is the case for staging them:

**The wrapper exemption admitted the very double decode it was added to
prevent.** Accepting a text wrapper as a source is right; *classifying its
output* is not, and the first draft did both. Sixteen UTF-16 LE code units
alternating ASCII with U+0000 decode to UTF-8 that alternates ASCII with a NUL
byte — which is exactly the pattern `sniff_ucs2` looks for. The outer wrapper
called it UCS-2 LE, decoded a second time and silently ate all eight NUL
characters. Worse, it was a *regression*: the old wire-read path re-sniffed the
raw UTF-16 and got it right, so §14.3's "strictly better than the old
behaviour" was false as written. Fixed by skipping classification entirely
over a wrapper source — there is nothing left to classify — and pinned by an
exact-bytes assertion that was RED against the draft.

Two contract defects rode along, both in docstrings the change had *edited*
without reconciling: `axl_compress_writer_finish` still promised plain
idempotence while the new unlatched refusal makes a second call succeed after
a first one failed; and nothing said that an implicit finalize through
`axl_fclose` now drops the **whole** payload when the sink is transcoding,
which `void` cannot report. Both are behaviour worth keeping and contracts
worth correcting rather than the reverse.
