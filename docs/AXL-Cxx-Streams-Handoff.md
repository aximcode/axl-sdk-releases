# Handoff — `axl::cout` / `axl::cin` / `axl::cerr`

> **Status: DONE 2026-08-07.** Implemented, both arches, `verify.sh` ALL GREEN.
> The §3 design question is resolved — see "§3 RESOLVED" below. The document
> is kept as the record of WHY, since the measurements in §1 are the expensive
> part and nothing has invalidated them.
>
> **What changed from the plan.** The work grew one deliverable the handoff did
> not anticipate: **`axl::string`**. §4 assumed `>>` had somewhere to put a
> token, and freestanding it does not — `<string>` is gated by
> `bits/requires_hosted.h`, so the exact configuration this layer targets has
> no owning string at all. `include/axl/axl-string.hpp` fills that, carrying
> `std::string`'s interface and forwarding the search family to
> `std::string_view` (which IS freestanding, so `find` / `substr` /
> `starts_with` are libstdc++'s own algorithms rather than reimplementations
> that drift). It shipped as a skin over `AxlString` and was re-shaped on
> 2026-08-08 to standalone-with-SSO — see §6c.
>
> That reverses a sentence in `axl-cxx.hpp` — "there is no `axl::vector` and no
> `axl::string` either". That claim rests on `std::string` being available,
> which is true only under `--hosted`; the premise did not cover the
> freestanding case. Both headers now say which half applies where.

Boilerplate: pure C library with a C++ layer; TDD; both arches; `verify.sh`
ALL GREEN before commit; never `git add -A` (the tree carries pre-existing
untracked `SCRATCH.txt` and several `docs/AXL-*.md`); push freely
(`feedback_push_freely_axl_sdk`).

---

## 1. The decision

Build **`axl::cout` / `axl::cin` / `axl::cerr`** — a small stream layer over
`axl_printf` / `axl_printerr` / `axl_readline`. Do **not** try to make
`std::cout` work.

### Why not `std::`

Measured, in this order, each step correcting the previous guess:

| Step | Result |
|---|---|
| Symbols `std::cout << 42` needs | **23, and ZERO are locale** |
| Breakdown | 11 `_Unwind_*` + `fputc fputs fwrite sprintf stderr malloc free realloc gettext __libc_single_threaded pthread_mutex_lock/unlock` |
| `std::cin >> x` | the **same 23** — not worse |
| Shim all 23 over AXL | **links**, 155 KB |
| Run it | **`#PF`** in `std::ostream::sentry::sentry`, `CR2 = -24` |

The fault is a null dereference: `std::cout` was never constructed.
`ios_base::Init` is **absent from the image entirely** (`nm` finds zero
`ios_base::Init` symbols). Why it is absent was not chased — `KEEP(.init_array)`
(fixed this session, §5) did **not** change it.

Two dead ends, do not repeat them:

- **Our own `std::streambuf` makes it WORSE — 65 symbols, not 23.** The cost is
  above the sink, not below it: `std::ostream`'s constructor calls
  `basic_ios::init()`, which constructs a `std::locale`. Supplying the sink
  buys nothing. `std::cout` is *cheaper* because it reuses libstdc++'s
  pre-instantiated objects.
- **`<iostream>` collides with `libaxl-cxx.a`.** It drags `functexcept.o`,
  `new_opv.o` and `new_handler.o`, which multiply-define our `operator new[]`,
  the five `std::__throw_*` stubs and `std::nothrow`. Linking needs
  `--allow-multiple-definition` — which silences exactly the error class that
  caught the `.init_array` and `.rela.dyn` bugs this week.

So `std::` costs the unwinder (a real project — see
`AXL-Cxx-Stdlib-Surface.md` §4 tier 2), plus `--allow-multiple-definition`,
plus probably real `FILE` semantics for `stdio_sync_filebuf`, and buys only
the spelling.

### Why this spelling

**`axl::cout`, not `axl::out`.** `axl::err` is *already* the error constructor
in `axl-cxx.hpp`:

```cpp
return axl::err(AXL_INVALID);          // std::unexpected<AxlStatus>
```

Declaring `axl::err` as a stream object is a hard compile error —
`"redeclared as different kind of entity"`. Mirroring `std::` sidesteps that
and makes porting hosted code an `s/std::/axl::/`. It also matches
`AXL-Cxx-Design.md` §6b ("lowercase `axl::`, mirroring the standard").

## 2. What is already proven

A working prototype ran under QEMU on x64:

```
axlout: hello 42 3.5 true
axlout: to stderr
```

**667 bytes** over a plain `axl_printf` program — 44461 vs 43794 — and
**freestanding** (no `--hosted`). The prototype is not committed; it lived in
the session scratchpad. Its shape:

```cpp
namespace axl {
class ostream {
public:
    constexpr explicit ostream(bool to_err) : m_err(to_err) {}
    ostream &operator<<(const char *s) { ... }
    ostream &operator<<(int v)         { ... }
    // ... char, unsigned, long long, double, const void *, bool
    ostream &operator<<(ostream &(*m)(ostream &)) { return m(*this); }
private:
    bool m_err;
};
inline ostream &endl(ostream &o) { o << "\r\n"; return o; }
inline ostream cout{false};
inline ostream cerr{true};
}
```

## 3. The one open design question — `>>` failure

`<<` is trivial. `>>` is not, because it must report failure while chaining
wants to return the stream, and this layer already committed to "errors are
values" (`AXL-Cxx-Design.md` §6).

Proposal, to confirm with Mike before implementing — **both, not a
compromise**:

```cpp
axl::cin >> port >> host;                 // chains; sticky fail state
if (!axl::cin) { /* one check covers the run */ }

auto port = axl::cin.read<uint16_t>();    // axl::result<uint16_t>
if (!port) return axl::err(port.error());
```

`explicit operator bool()` for the chained path, `read<T>() ->
axl::result<T>` for the checked one. Same shape as `arena_allocator`: the
ergonomic path stays ergonomic, the path that must handle failure gets a value
to branch on.

Sub-questions that need answers before the implementation is written:

- Does `>>` skip leading whitespace like `std::`, and what is a token
  boundary — whitespace only, or newline for `axl::cin >> line`?
- On a parse failure, is the input consumed or pushed back? `std::` leaves the
  stream positioned at the offending character; `axl_readline` is line-oriented
  and gives no natural pushback.
- Does `cin` read a full line and tokenize (matches `axl_readline`), or read
  character-wise (matches `axl_console_read_key` and allows interactive
  editing)? The first is far simpler and probably right.

### §3 RESOLVED — both paths, one sticky state

Confirmed with Mike 2026-08-07 and implemented. The proposal above was taken
with one addition, and the three sub-questions answered as follows.

**Both paths, sharing state.** `explicit operator bool()` for the chained form,
`read<T>() -> axl::result<T>` for the checked one — and **`read<T>()` sets the
same sticky bit**. That addition is load-bearing, not tidiness:
`axl_str_to_u64()` REWINDS its `endptr` to the start on overflow (documented in
`axl-str.h`, and deliberately unlike `axl_str_to_double()`, which leaves it past
the digits). A cursor driven off `endptr` therefore makes NO progress on
`"99999999999999999999"`, so `while (auto v = cin.read<int>())` would spin
forever without a bit to stop it. `clear()` resets.

**(a) Whitespace and token boundaries** — skip leading whitespace; a token runs
to the next whitespace; `\n` is ordinary whitespace, so `cin >> a >> b` spans
lines exactly as `std::cin` does. Almost free: `axl_str_to_u64` already skips
leading whitespace and hands back an `endptr`, and its own docs call partial
parsing "what a tokenizer wants". Text extraction went to a constrained
template over `assign(p, n)` / `clear()` (`axl::byte_sink`) plus a
`char (&)[N]` overload — the array reference makes the overflow
`std::istream::operator>>(char *)` allows unrepresentable.

**(b) Pushback** — free, and `std::` semantics reproduce exactly. `cin` owns
the line buffer, so "pushback" is a cursor rewind, not a stream operation. On a
parse failure the cursor stays AT the offending character and the target is
left unmodified. One deliberate divergence: `std::` stores a saturated value on
an out-of-range number, and this does not, because AXL's integer parsers report
syntax and range errors identically — a saturated value would be
indistinguishable from a real one.

**(c) Line-oriented**, and the reason is stronger than "simpler": character-wise
via `axl_console_read_key` cannot see a redirect AT ALL, so it would be both
wrong for pipes and untestable — a shell redirect is the only way to feed a
UEFI image stdin. `axl_readline` covers redirect, both pipe forms, and the
interactive console line editor through one path. The source is
`axl_stdin_text()` (lazy, freed via `axl_atexit`), which undoes the UEFI
Shell's UCS-2 transcode on the default `|`; reading raw `axl_stdin` would make
`echo 42 | prog` parse as `4`.

## 4. The work

1. `include/axl/axl-ostream.hpp` — `axl::ostream`, `axl::cout`, `axl::cerr`,
   `axl::endl`. Freestanding.
2. `include/axl/axl-istream.hpp` — `axl::istream`, `axl::cin`, over
   `axl_readline` + `axl_str_to_u64/s64/...` (`include/axl/axl-str.h`).
3. Tests in `test/integration/test-cxx-hosted-qemu.sh` — it already builds and
   RUNS freestanding C++ fixtures; follow the `cxx-errors` / `cxx-ctor`
   pattern. Exact-string assertions (`grep -Fxq`), per CLAUDE.md bucket B.
4. An SDK example, wired into `sdk/examples/CMakeLists.txt`. Freestanding, so
   no `axl-example: hosted` marker needed.
5. Docs: `docs/sphinx/modules/cxx.rst` gets `.. doxygenfile::` entries for both
   headers, or its own page (`make check-docs` fails without one), plus a
   `src/*/README.md` pass. `./scripts/build-docs.sh` is a zero-warning gate.

## 5. Traps

**Make the stream objects CONSTANT-INITIALIZABLE.** Give the constructor
`constexpr` so `inline ostream cout{false};` lands in `.data` with no
`.init_array` entry. Verified: a `constexpr` ctor emits no dynamic
initializer, a non-`constexpr` one emits `_GLOBAL__sub_I`. This matters twice
— it removes any static-init-order question, and it is why the prototype
worked *before* the `.init_array` bug below was fixed.

**`.init_array` was collected by `--gc-sections` until `8db522c1`.** No C++
global constructor in any image had ever run. Fixed with `KEEP()` in both
linker scripts, guarded by `test/integration/cxx-ctor-selftest.cpp`. Relevant
here only if you give a stream a non-`constexpr` constructor — don't.

**A comment claiming coverage is not coverage.** That bug survived because
`sdk/examples/hello.cpp` said *"Static initializer — proves .init_array runs
before main()"* over `const char *const kDefaultName = "world"` — a CONSTANT
initializer, which emits no constructor at all.

**The prototype's `emit()` defeats `-Wformat`.** It forwards a runtime format
string to `axl_printf` via a variadic template. Production shape wants explicit
per-type calls with literal formats, or clang-tidy and `-Wformat` see nothing.

**Decide `double` formatting deliberately.** The prototype used `%g`. AXL has
its own float formatting; pick one and pin it with an exact-string test.

**The staged SDK is a SECOND TREE.** `axl-c++` compiles against
`out/include/axl-sdk` and links `out/lib`. Editing `include/axl/*.hpp` without
re-running `scripts/install.sh --arch all --cpp` means you are testing the
previous build. This produced a false sabotage verdict in the last session;
`test-cxx-hosted-qemu.sh` now asserts the staged headers match, but only for
`include/axl`.

**`set -o pipefail` + `grep -q` inverts assertions.** `grep -q` exits on its
first match and SIGPIPEs the producer, so `cmd | grep -q X` reports FAILURE
exactly when it FOUND X. This bit twice in one session and silently passed a
sabotage. Write to a file, then grep the file.

**Three build paths, and they drift.** Makefile, `scripts/axl-cc`, and the
CMake package generated by a heredoc in `scripts/install.sh`. `make
check-flag-parity` asserts they agree on ABI/safety flags and the `objcopy -j`
list. Note `lib/cmake/axl/axl-config.cmake` in the working tree is a
**gitignored stale artifact** and NOT the source — editing it does nothing.

**A CFLAGS change now forces a rebuild** (`$(BUILDDIR)/.axl-build-state`), so
the old "edit flags, nothing rebuilds, draw a wrong conclusion" trap is gone.
`$(BUILDDIR)/.axl-flags` holds the `CC`/`CXX` pair followed by the exact flags
the neighbouring objects were built with — read it first when a build looks
wrong. (Changing the COMPILER invalidates them too, since it joined the
signature.)

**`CXXFLAGS` had no `-MD -MP` until this work** (§6b, bug 4). The C side always
had them. If you are chasing a C++ change that seems to have no effect on a
tree older than 2026-08-07, that is why — and note the signature file above
covers WHICH FLAGS were used, not which HEADERS an object depends on, so it
would not have caught this.

**`scripts/install.sh` stages RELEASE only, where `AXL_MEM_DEBUG` is off.** So
an image built by `axl-c++` produces NO leak accounting whatsoever, and "no
leak report" from one means "nothing was watching". The streams test therefore
runs a Makefile-built DEBUG fixture and keeps `axl-c++` as a separate
compile+link assertion. It also asserts a MINIMUM count of clean
`mem: no leaks detected` verdicts, so the gate fails loudly if it ever goes
blind again.

## 6. Repro commands

```sh
# stage first — the staged SDK is what axl-c++ uses
./scripts/install.sh --arch all --cpp

# freestanding C++ build + run
out/bin/axl-c++ --arch x64 --release prog.cpp -o prog.efi
./scripts/run-qemu.sh --arch X64 --timeout 45 prog.efi

# the C++ suite (106 assertions, both arches)
./test/integration/test-cxx-hosted-qemu.sh both

# gates
./scripts/verify.sh                       # 13 gates, both arches
make AXL_CPP=1 check-no-avx
make check-reloc-coverage check-flag-parity
```

## 6b. What shipped, and the four latent bugs it surfaced

**On the byte figures.** §2's 667 is the prototype's, against its own baseline,
and it stands as that. Do not carry any of these numbers forward as constants:
`libaxl.a` is selectively linked, so a "cost" here is the difference between
two DIFFERENT sets of pulled objects, and it moves whenever the library does.
The shipped `axl::cout` measured 1227 bytes and then 715 across one
afternoon — the delta shrank while the code GREW, because the `axl_printf`
baseline grew faster. A precise byte count is the wrong shape for a durable
doc claim; an order of magnitude plus the conditions is the right one. (This
document said 667 in four other places for a while because that lesson had to
be learned twice.)

Delivered: `include/axl/axl-string.hpp`, `axl-ostream.hpp`, `axl-istream.hpp`;
six new `AxlString` C accessors (`data`, `capacity`, `reserve`,
`shrink_to_fit`, `resize`, `insert_len`); `sdk/examples/cxx-streams.cpp`;
`test/integration/test-cxx-streams-qemu.sh` (50 assertions per arch, driven
from `startup.nsh` so `cin` gets real redirected and piped stdin); three Sphinx
pages. All three stream globals verified constant-initialised — `readelf`
shows no `.init_array` section and `nm` no `_GLOBAL__sub_I`.

Four bugs that predate this work, each found by trying to build on it:

1. **`axl_string_steal()` then any append SPUN FOREVER.** `steal` sets
   `alloc = 0`; `grow` sized the replacement as `new_alloc *= 2`, and `0 * 2`
   is `0`. Under UEFI that is a wedged image, not a failed call. The README had
   promised "the builder can be reused after stealing" the whole time — the
   prose was right and the code was wrong.
2. **`b->len + need + 1` could wrap**, so `grow` returned success without
   resizing and the caller's `memcpy` ran off the heap block.
3. **`s += s.str()` read freed memory.** The source pointed into the buffer
   `grow` had just reallocated. Same hazard in `insert`, where the shift also
   scrambles the source. Both now route through a private copy.
4. **`CXXFLAGS` carried no `-MD -MP`** — for as long as the C++ layer has
   existed. No `.cpp` object had ANY header dependency, so editing `axl-cxx.hpp`
   or any other `.hpp` rebuilt nothing. This is a wrong-answer generator, not a
   slow-build annoyance: it made a sabotage of `axl-istream.hpp` report as
   UNDETECTED because the code was never recompiled. It is the C++ twin of the
   `CFLAGS`-signature trap already recorded in CLAUDE.md.

### What the pre-commit review added

Two independent reviewers (C layer, C++ layer) ran against the finished work,
both confirming findings under AddressSanitizer/valgrind rather than by
reasoning. They found **13 more bugs**, and the interesting thing is that the
suite was green when they started — these are all things tests alone did not
catch.

Four were REGRESSIONS this work introduced, and one class stands out: fixing
the `grow()` spin made the buffer-less paths *succeed* where they used to
hang, but nothing wrote the terminator into that first allocation. A hang
became a heap overread — worse, because it is silent. `reserve()` and all
three `prepend` variants were affected. The lesson generalises: **when you fix
a "this never returns" bug, ask what the newly-reachable code assumes.**

The aliasing guard was the other half. `append`/`insert` were guarded;
`prepend`, `prepend_len`, `overwrite` and `append_printf` were not, and each
was a confirmed use-after-free. `append_printf` needed two guards, because
`axl_vformat` hands the writer the caller's `%s` pointers *and* keeps parsing
the format string across those calls — so a self-referencing `fmt` dangles
even when every chunk is copied.

On the C++ side, three were silent data corruption with `bad()` reading clean:
`assign()` cleared before copying from a source that could be its own buffer
(`s = s.c_str()` gave `"\0ello"`); `replace()` captured `v.size()` before the
erase shifted the bytes out from under it, and — as the only COMPOUND mutator
— could also break the file's own "unchanged on OOM" promise by failing
between its two halves; and `append(n, c)` computed `size() + n` in C++, where
it wraps *before* `grow()`'s overflow guard can see it, arriving as a
truncation request.

Two more were pure "nothing compiles this" (`feedback_uncompiled_code_is_a_bug_class`):
`operator+(const string &, const string &)` was an AMBIGUITY error — the most
obvious call in the whole API — and `axl::cout << axl::hex` compiled to
`operator<<(bool)` and printed `true`, because a one-directional manipulator
still converts to a function pointer. Both now have callers in the fixture,
which is what would have caught them.

The test suite gained a verb (`edge`, 16 exact lines) and the assertion the
script's own header had CLAIMED and never made: that the stream globals emit
no `.init_array` entry. That is the same "a gate that cannot see is worse than
none" shape as the leak check beside it, and both now carry positive controls.

### Symbols the freestanding link needs

Two symbols had to be supplied for freestanding `std::string_view` to link at
all, and neither shows up until something actually CALLS a search function:
`memchr` (in `axl-str-compat.c` — `char_traits<char>::find` calls it out of
line) and `std::terminate` (in `axl-cxxabi-ops.cpp`, beside the five
`std::__throw_*` stubs, for the same reason: letting `libstdc++.a` supply it
pulls `eh_terminate.o` and the unwinder behind it). `std::terminate` must NOT
be re-declared locally — `<bits/c++config.h>` already declares it
`__noreturn__`, and repeating it puts `[[noreturn]]` on a non-first
declaration, which gcc accepts and clang rejects.

## 6c. The re-shape: axl::string is not a skin

It shipped as a wrapper over `AxlString`, and `AXL-Cxx-Design.md` §4.5 had
ALREADY measured that shape and rejected it — the section is titled "GO for
the structure, NO-GO for the skin". I did not read it before choosing. The
handoff and the surface doc were consulted; the design doc's §4.5 was sitting
there with the answer.

§4.5, N=200000, warm heap, microseconds:

| shape | ctor (short) | copy (short) |
|---|--:|--:|
| skin over `AxlString` | 29055 | 29336 |
| standalone, always heap | 13749 | 14481 |
| **standalone, SSO** | **3142** | **3034** |

The control is `ctor LONG`, which forces an allocation in every shape and
converges at ~300 ms for all four — proof that the entire difference is
allocation AVOIDANCE. A skin cannot express it: `AxlString`'s handle is a
pointer to a heap object, so there is nowhere to put the inline bytes.

Re-measured on THIS tree rather than inherited — same benchmark before and
after, one box, `\EFI\BOOT\BOOTX64.EFI` (21 bytes): ctor 33279 -> 4790 us
(**6.9x**), copy 31120 -> 3324 us (**9.4x**).

`sso_capacity` is **23**, not `std::string`'s 15, and the reason is that
21-character path: a 15-byte inline buffer spills the most common string in
the tree onto the heap. The cost is 48 bytes per string against libstdc++'s
32. The pointer is always valid and self-referential exactly while the content
is inline, so reads never branch and there is no empty-string special case —
which is how `data()` and `c_str()` keep the identity `std::string` promises.

One test caught the change honestly: `edge: oom-lazy` appended four bytes to
force an OOM, and after SSO four bytes no longer allocate, so it could not
fail. It had stopped testing OOM the moment the optimisation worked. The probe
now exceeds `sso_capacity` deliberately.

`AxlString` keeps the job §4.5 measured it TIED on — the streaming builder
behind the JSON and XML writer sinks. Its six new accessors
(`data`/`capacity`/`reserve`/`shrink_to_fit`/`resize`/`insert_len`) were added
for the skin and now have no production caller, only their own tests. They are
kept as a coherent completion of the builder surface, but that is a judgement
call worth revisiting rather than a demonstrated need.

## 7. Related docs

- `AXL-Cxx-Stdlib-Surface.md` — the measured STL surface: what works
  freestanding, what needs `--hosted`, and what each remaining "no" costs in
  symbols. §6 is the `std::cout` measurement summarised above.
- `AXL-Cxx-Design.md` — §2b.1–2b.4 for what the C++ layer supplies and why
  both `-fno-stack-protector` and `-fno-rtti` turned out not to be forced;
  §8 for the two decisions that are Mike's.
- `AXL-Cxx-Stdlib-Handoff.md` — the predecessor, now DONE.
