# axlmm — Design

**Status:** **design complete; implementation deferred pending AGT
consumer validation.**  Phase 1 (CPP1.0–1.6) validated the C++
toolchain end-to-end and shipped the C++ ABI runtime
(`libaxl-cxx.a`, `__cxa_atexit`, operator new/delete,
`__cxa_pure_virtual`, `.init_array` walker, axl-c++ wrapper).  The
wrapper class library described here (CPP1.7 onward) is parked until
AGT exists as a real consumer that can validate the API shape.

This doc captures every locked-in design decision so implementation
can resume at any point without re-deriving them.

## Goal

A C++ wrapper layer on top of axl-sdk's C public surface, modeled on
glibmm's relationship to glib.  Adds C++ ergonomics (RAII, type
safety, range-for, `std::expected`) without changing what the C
library does or how it does it.

## Non-goals

- **Not a parallel implementation.**  Every axlmm operation routes
  to the underlying C function.  axlmm never reimplements logic that
  lives in C.  When axlmm needs a capability the C side lacks (e.g.
  an iter API for a callback-only foreach), the capability is added
  to the C side first; axlmm just wraps what's there.
- **Not a replacement for the C surface.**  axl-sdk remains a C
  library.  axlmm consumers can freely mix C calls (`axl_printf(...)`)
  and C++ wrapper calls (`stream.write(...)`); they cost the same at
  runtime.
- **Not a port of glibmm/sigc++/etc.**  We share their layering shape
  (C library + thin C++ wrapper) but not their type system, signal
  machinery, or runtime overhead.
- **Not a separate repo.**  Lives in axl-sdk alongside the C surface.
  The glibmm/glib repo split exists because they have different
  release cadences, different maintainer teams, and many sibling
  bindings (PyGObject, vala, gjs).  None applies to axl-sdk.

## Why axlmm exists (and what it isn't)

axlmm provides **ergonomic enhancements, not capabilities.**  AGT
or any other C++ consumer can call the axl-sdk C API directly:

```cpp
AXL_AUTOPTR(AxlStream) s = axl_stream_open("/dev/null");
if (!s) return -1;
int rc = axl_stream_write(s, "hello", 5);
if (rc != AXL_OK) return rc;
axl_stream_flush(s);
// auto-freed at scope exit via GCC cleanup attribute
```

This works today.  No axlmm needed.  `AXL_AUTOPTR` is a GCC cleanup
attribute that g++ supports natively; ~14 axl-sdk types already
register it via `AXL_DEFINE_AUTOPTR_CLEANUP(Type, free_fn)`.

axlmm wraps the same call as:

```cpp
auto sr = axlmm::Stream::open("/dev/null");
if (!sr) return -1;
auto s = std::move(*sr);
s.write("hello");
s.flush();
if (s.status() != axlmm::Status::OK) return -1;
```

The win is aesthetic:
- `.write()` method syntax vs `axl_stream_write(s, ...)`
- Sticky-error chain (no per-call `if (rc != AXL_OK)`)
- `std::string_view` overloads (no `.data(), .size()` pair)
- `std::expected` factory returns
- Type-safe `Status` enum vs untyped `int`

Across a 10,000-line C++ codebase that's not transformative — it's
just nicer.  The honest assessment is: axlmm exists to make C++
consumer code read as idiomatic C++ rather than "C++ pretending to
be C", and to encode the C API's error/ownership conventions in the
type system so consumers can't get them wrong.

## Implementation status

Phase 1 of the axlmm rollout (toolchain validation + C++ ABI
runtime) is **complete and shipped.**  This includes:

- `axl-c++` wrapper driver (+ `axl-cc` extension dispatch)
- `libaxl-cxx.a` archive with operator new/delete/etc. routing to
  `axl_malloc`/`axl_free`
- `__cxa_atexit` shim (→ `axl_atexit`), `__dso_handle`,
  `.init_array` walker in `src/runtime/axl-cxxabi.c`
- `__cxa_pure_virtual` stub in `src/runtime/axl-cxxabi-ops.cpp`
- AArch64 `-ffixed-x18` fix (UEFI ABI compliance — independent of
  C++ work but surfaced during validation)
- `scripts/install-arm-toolchain.sh` for the ARM bare-metal
  `aarch64-none-elf-g++` cross
- `install.sh` auto-detects the C++ toolchain and builds the
  C++ variant when present

The wrapper class layer (CPP1.7 onward) is **deferred until AGT
exists** as a real consumer.  Rationale:

- glibmm came AFTER glib had real consumers (glib 1998 → glibmm
  2002; four years of glib evolution informed the wrapper design)
- Designing axlmm wrappers without a consumer risks wrapping the
  wrong things or wrapping them wrong — rework cost is real
- AGT will surface usage patterns we can't predict from the C
  surface alone (which methods are hot, which need overloads, which
  factory shapes are convenient, what iteration looks like in
  practice)
- The toolchain work that AGT actually needs is done; the wrapper
  layer is optional polish on top

**When implementation resumes:** this doc is the spec.  The
decisions below are settled; revisit them only if AGT's actual
patterns invalidate them (and document the change if so).

## Toolchain & constraints

Established by Phase 1 validation; documented here for the record.

**Compilers:**
- AArch64: `aarch64-none-elf-g++` 14.3.Rel1 (ARM developer.arm.com
  bare-metal cross).  Version-pinned to match a downstream UEFI-C++
  consumer's choice so axl-sdk-built and consumer-built C++ share an ABI.
- X64: host `g++` (matches axl-cc's host-gcc convention)
- The Linux-ABI cross (`aarch64-linux-gnu-g++`) is NOT viable — its
  libstdc++ headers pull hosted typedefs from `<bits/c++config.h>`

**Compile flags (hard defaults in axl-c++):**

```
-ffreestanding -fshort-wchar -fno-builtin
-fno-stack-protector -fno-omit-frame-pointer
-fno-exceptions -fno-rtti -fno-threadsafe-statics
-ffunction-sections -fdata-sections -fpic
-std=c++20
+ per-arch: -ffixed-x18 (aa64), -mno-red-zone -march=x86-64 (x64)
```

Consumers cannot opt in to exceptions, RTTI, or thread-safe statics
— the link won't satisfy libsupc++ symbols (validated in CPP1.3/1.4;
matches every freestanding-UEFI C++ project's experience).

**Link flags:** `-nostdlib --no-undefined`.

**Required C++ standard: C++20** (for `<span>`, `<concepts>`,
`<string_view>::starts_with()`, the floor of the libstdc++ subset we
use).

**What works** (header-only libstdc++ subset, verified in CPP1.5
plus P0829 freestanding subset):

- Always-freestanding: `<type_traits>`, `<utility>`,
  `<initializer_list>`, `<new>`, `<cstddef>`, `<cstdint>`,
  `<limits>`, `<bit>`, `<concepts>`, `<compare>`,
  `<source_location>`
- Practically usable: `<array>`, `<span>`, `<string_view>`,
  `<tuple>`, `<optional>`, `<variant>`, `<expected>` (C++23),
  header-only subsets of `<algorithm>` / `<numeric>` /
  `<functional>`
- Use with caution: `<chrono>` (some hosted backends), `<atomic>`
  (may need `-latomic` for 128-bit ops on AArch64), `<ranges>`
  (compile-time heavy)

**What doesn't work** (require libsupc++ / libstdc++.a):

- Exceptions (`throw`/`catch`) — `__cxa_throw`, `_Unwind_Resume`,
  `__gxx_personality_v0` unresolvable
- RTTI (`typeid`, `dynamic_cast`) — `__dynamic_cast` +
  `__cxxabiv1::__*_type_info` vtables unresolvable
- `<string>`, `<vector>`, `<unordered_map>`, etc. — libstdc++
  allocator + exception-throwing container code
- `<stdexcept>`, `std::runtime_error` — chain to exception
  machinery
- `<format>` — not in the C++23 freestanding subset (P0829); its
  public API requires pieces of libstdc++ that aren't header-only
  (allocator-interacting string code, `std::format_error`, locale
  facets).  The precise unresolved-symbol list is unknown without
  testing; a CPP1.5-style validation pass is the way to find out
  if/when a consumer needs it.
- `thread_local` — needs `__tls_get_addr` / `__emutls_*`
  unresolvable.  UEFI is single-threaded anyway.

## Design decisions

### Naming convention

Drop the `Axl` prefix.  The namespace conveys it.

```cpp
namespace axlmm {
    class Stream { ... };
    class Event { ... };
    class Cancellable { ... };
    class StrBuf { ... };
    class Arena { ... };

    enum class Status { OK, ERR, CANCELLED, TIMEOUT, NO_MEMORY };
}

axlmm::Stream s;
axlmm::Status st = s.status();
if (st == axlmm::Status::OK) { ... }
```

Matches the universal convention in C-wrapped-as-C++ space (glibmm
`Glib::Object` not `Glib::GObject`; gtkmm, sigc++, atkmm all drop
the prefix).  Enum values follow the same rule: `Status::OK` not
`Status::AXL_OK`.

**Header file naming** follows the same convention: kebab-case
without the `axl-` prefix.

```
<axlmm/stream.hpp>      wraps <axl/axl-stream.h>
<axlmm/event.hpp>       wraps <axl/axl-event.h>
<axlmm/cancellable.hpp>
<axlmm/strbuf.hpp>
<axlmm/arena.hpp>
<axlmm/status.hpp>      public — Status enum + helpers
<axlmm/detail/handle.hpp>  internal — RAII handle template
<axlmm.hpp>             umbrella
```

### Error returning shape

**Sticky-error model on methods; `std::expected` from factories.**

Method calls return `void` (or `T` for accessor-style); failures
latch onto a per-instance `Status status_` field accessed via
`Status status() const noexcept`.  Subsequent calls no-op when
`status_ != Status::OK`.

```cpp
axlmm::Stream s = axlmm::Stream::open("file").value();
s.write("hello");
s.write(", world");      // no-op if previous failed
s.flush();
if (s.status() != axlmm::Status::OK) {
    handle_error(s.status());
}
```

Mirrors the C-side convention used by `AxlString`, `AxlJsonWriter`,
`AxlXmlWriter`.  Lightweight (Status is a 4-byte enum stored in the
wrapper), composable (chains don't need per-call `if (!rc)` checks),
familiar (consumers who know one AXL builder know all of them).

**Factories return `std::expected<T, axlmm::Status>`** because they
have no prior state to latch into:

```cpp
auto result = axlmm::Stream::open("file");   // expected<Stream, Status>
if (!result) return result.error();
auto s = std::move(*result);
```

### Handle ownership

**Move-only RAII handle template, default-construct allowed,
`.clone()` only on ref-counted types.**

```cpp
// <axlmm/detail/handle.hpp> — internal scaffold (~50 LOC)
namespace axlmm::detail {
template <typename T, void (*Deleter)(T*)>
class Handle {
public:
    Handle() noexcept : ptr_(nullptr) {}           // empty default
    explicit Handle(T *ptr) noexcept : ptr_(ptr) {}
    ~Handle() { if (ptr_) Deleter(ptr_); }
    Handle(Handle&&) noexcept;
    Handle& operator=(Handle&&) noexcept;
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    T* get() const noexcept { return ptr_; }
    T* release() noexcept;                          // pass ownership to C
    bool valid() const noexcept { return ptr_ != nullptr; }
    explicit operator bool() const noexcept { return valid(); }
private:
    T *ptr_;
};
}
```

**Move-only** because most C types use the simple new/free
ownership pattern (caller allocates, caller frees).  No runtime
overhead.  Mirrors `std::unique_ptr`.

**Default-construct allowed** (creates an empty wrapper with
`status() == Status::ERR`) because pre-1.0 we'll discover container
needs we haven't anticipated; banning default-construct is a tax
that doesn't pay off.  Factories use `std::expected` so the
"always valid after construction" property is preserved on the
happy path.

**Ref-counted types** (the few that have `_ref`/`_unref` in the C
API: AxlLoop, AxlHttpServer) use the same template with `_unref`
as the deleter.  The wrapper owns ONE ref count, drops on
destruction.  Shared semantics opt-in via explicit `.clone()`
(calls `_ref` to add a ref, returns a new wrapper).  Non-ref-counted
types have no `.clone()` method — discoverable via the API itself.

**Note: `AXL_AUTOPTR` is a complementary lightweight RAII path** for
C++ consumers who don't want the full axlmm wrapper.  The same
GCC cleanup attribute that gives C scope-bound free works in g++
unchanged.  Consumers choose: full axlmm wrapper for method
syntax + sticky error + factory expected, OR raw C pointer +
AXL_AUTOPTR for minimal-overhead RAII without the wrapper layer.

### Status enum

Flat enum mirroring the C side, with translation helpers.

```cpp
namespace axlmm {

enum class Status : int {
    OK         = 0,    // matches AXL_OK
    ERR        = -1,   // matches AXL_ERR (catch-all)
    CANCELLED  = -2,   // matches AXL_CANCELLED
    TIMEOUT    = -3,   // matches AXL_TIMEOUT
    NO_MEMORY  = -4,
    // grows as concrete cases emerge
};

constexpr Status status_from_int(int rc) noexcept {
    switch (rc) {
        case AXL_OK:        return Status::OK;
        case AXL_ERR:       return Status::ERR;
        case AXL_CANCELLED: return Status::CANCELLED;
        case AXL_TIMEOUT:   return Status::TIMEOUT;
        default:            return Status::ERR;
    }
}

const char *status_to_string(Status s) noexcept;
// "OK", "ERR", "CANCELLED", "TIMEOUT", "NO_MEMORY", ...
}
```

Values match the C side exactly so `static_cast<int>(Status::OK)`
round-trips.  Stays flat (one type, not per-module variants) to
preserve the "single Status everywhere" property that makes the
sticky-error model uniform.  Additive growth is backward compatible.

### Iteration (range-for adapters)

**Hand-rolled forward iterators per type; require explicit C iter
API; callback-only types deferred.**

For types with explicit `_iter_*` API (today: AxlHashTable; future:
AxlArray, AxlPci, AxlUsb after they grow iter API on the C side):

```cpp
class HashTable {
public:
    class Iter {
        AxlHashTableIter raw_;
        void *key_, *value_;
        bool done_;
    public:
        Iter(AxlHashTable *t);              // _init + first _next
        Iter();                              // end sentinel
        Iter& operator++();
        auto operator*() { return std::pair{(const char*)key_, value_}; }
        bool operator!=(const Iter& o) const { return done_ != o.done_; }
    };
    Iter begin() { return Iter{handle_.get()}; }
    Iter end()   { return Iter{}; }
};

// Consumer:
for (auto [key, value] : my_table) { ... }
```

- Iterator concept: **InputIterator** (single-pass, no `operator--`,
  no random access) — matches what the underlying C iter API
  provides
- End iterator is a **default-constructed sentinel**; comparison via
  `done_` flag
- Iterator `value_type` deduced from `operator*`; for maps that's
  `std::pair<key, value>` (header-only `<utility>`)
- **No fake const-iterator** — C side doesn't distinguish const vs
  mutable iteration; pretending otherwise would mislead

For **callback-only types** (AxlSidecar, AxlList foreach), axlmm
does NOT expose `begin()`/`end()` until the C side adds explicit
iter API.  Consumers use the underlying `axl_*_foreach(handle, cb,
ud)` directly with a C++ lambda.  Adding C-side iter API is good
hygiene anyway — not an axlmm-only concern.

### printf / format wrappers

**Deferred entirely.**  CPP1 ships NO printf wrappers.

Consumers call C printf family directly from C++:

```cpp
axl_printf("Stream open: %s\n", path);
axl_printf("count=%d name=%s\n", count, name);
```

This works today because all axl headers wrap their declarations in
`#ifdef __cplusplus extern "C" { ... }`.

`std::format` would be the idiomatic answer but isn't in the C++23
freestanding subset (P0829) — its public API references libstdc++
pieces that aren't header-only (allocator-interacting string code,
`std::format_error`, locale facets).  If a consumer asks for
type-safe format, a CPP1.5-style validation pass would test
`<format>` end-to-end with `--no-undefined` to determine the actual
unresolved-symbol surface.  Until then, the C printf family is the
answer.

A future `axlmm::format` module is parked as an open question; out
of scope for v0.1.

### Header layout

- **One header per wrapped C type:** `axlmm/stream.hpp`,
  `axlmm/event.hpp`, etc.  Mirrors the C side 1:1.
- **`axlmm/status.hpp`** (public) — Status enum + `status_from_int`
  + `status_to_string`.  Included by every wrapper.
- **`axlmm/detail/handle.hpp`** (internal) — Handle template.
  Promote to `axlmm/handle.hpp` (public namespace) if a vendor-
  extension consumer emerges asking to wrap their own C handles.
- **`axlmm.hpp`** — umbrella header that includes everything.
  Mirrors `<axl.h>` on the C side.

**Standard include template per wrapper header:**

```cpp
#ifndef AXLMM_X_HPP
#define AXLMM_X_HPP

#include <axl/axl-X.h>                  // wraps this C header
#include <axlmm/status.hpp>             // every wrapper uses Status
#include <axlmm/detail/handle.hpp>      // every wrapper uses Handle
#include <expected>                     // factories return std::expected
// ... any other stdlib pieces (utility, string_view, etc.)

namespace axlmm {

class X {
    // ...
};

}

#endif
```

**Header-only implementation with `inline` keyword.**  Each
`axlmm/X.hpp` contains both declaration and definition.
`libaxl-cxx.a` stays pure C++ ABI shims (operator new/delete,
`__cxa_pure_virtual`) — no wrapper .cpp files contribute to it.
Templates require this (Handle template can't be split without
explicit per-type instantiation).  For non-template wrappers, the
`inline` keyword guarantees ODR safety; the compiler chooses to
inline at call site or emit a weak symbol.

If compile time or ABI surface becomes a concrete concern later,
individual functions can split to `src/axlmm/X.cpp` and join
`libaxl-cxx.a`.  YAGNI for v0.1.

### Testing

- **Test framework:** reuse `test/unit/axl-test.h` directly.
  Header-only `static inline` helpers (`test_pass`, `test_fail`,
  `test_check`).  Compiles fine in C++ without modification.
  Building a parallel C++ test framework would be ~500 LOC for
  ergonomic gain the existing macros already provide.

- **Test file naming:** `test/unit/axlmm-test-*.cpp`.  Different
  prefix from `axl-test-*.c` so a glob can attribute test counts
  separately if we ever need that.

- **Ratchet:** single unified ratchet in
  `test/integration/.last-pass-count` for v0.1.  The current
  machinery counts PASS lines and bumps on green runs; adding C++
  test PASS lines to the same count works fine.  Split to per-tier
  ratchets later if axlmm grows enough to warrant attribution; cost
  of splitting later is one snapshot + a sed.

- **`make tests` requires `AXL_CPP=1`.**  The ratchet is an
  SDK-development concern, not a consumer concern.  SDK developers
  have the C++ toolchain installed (per
  `scripts/install-arm-toolchain.sh`) — that's the canonical dev
  environment.

- **First tests:** `axlmm-test-handle.cpp` (Handle template:
  default-construct empty, construct with raw, move construction,
  move assignment, destructor calls deleter, release, get, valid,
  operator bool, double-free safety) and `axlmm-test-status.cpp`
  (Status enum: status_from_int round-trip, status_to_string for
  all values).  Per-wrapper tests land alongside each wrapper.

### Packaging

**Single `axl-sdk.deb`/`.rpm` per arch per release.**  C and C++
surface ship together.

The build-time `AXL_CPP` gate stays for the source-from-scratch
case (developer building axl-sdk on a machine without the ARM
bare-metal toolchain).  But the official distro packages always
include the C++ bits — CI builds with the toolchain cached, so
release artifacts always have full coverage.

Rationale (revised from the original ROADMAP's two-package plan):
- `libaxl-cxx.a` doesn't link libstdc++ (validated in CPP1.3-1.5;
  no libsupc++, no libstdc++.a).  The base package's declared
  dependencies don't change when C++ files are included.
- Size bloat is ~80-100KB (axlmm headers + libaxl-cxx.a + axl-c++
  wrapper + axl-cpp.pc) against an existing package of several MB.
  Inconsequential.
- Pure-C consumers ignore the .hpp headers, the .a, the wrapper.
  They pay no runtime cost.
- Single repo, single release cycle, single maintainer — package
  split would be cargo-culting Debian conventions without a real
  reason.

**Detection mechanism for the source-from-scratch edge case:**
`pkg-config --exists axl-cpp` (new `.pc` file ships only when
`libaxl-cxx.a` is in the install).  Or check
`[ -f /usr/lib/axl/<arch>/libaxl-cxx.a ]`.  Or rely on axl-cc's
existing helpful error when you feed `.cpp` to a no-libaxl-cxx.a
install.

**ARM bare-metal toolchain stays out of the package.**
`scripts/install-arm-toolchain.sh` is a separate user-invoked step
— bundling a 96MB tarball into a deb would mean owning the
toolchain's release cycle and license redistribution.

## Implementation roadmap (when deferral lifts)

When AGT exists and has validated which axlmm shapes actually pay
off, implementation resumes per the sub-phases below.  Each is
independently testable and ships as its own commit.

### Phase CPP1.7a — Foundation + AxlStream (deferred)

The smallest viable end-to-end landing.  Ships:

| File | Purpose |
|---|---|
| `include/axlmm.hpp` | Umbrella (just Stream initially) |
| `include/axlmm/status.hpp` | Status enum + helpers |
| `include/axlmm/stream.hpp` | AxlStream wrapper |
| `include/axlmm/detail/handle.hpp` | Handle template |
| `test/unit/axlmm-test-handle.cpp` | Handle template tests |
| `test/unit/axlmm-test-status.cpp` | Status enum tests |
| `test/unit/axlmm-test-stream.cpp` | Stream wrapper tests |
| `sdk/examples/hello.cpp` | End-to-end demo |
| Makefile changes | C++ test build rule + libaxl-cxx.a test linking |
| install.sh changes | Stage axlmm headers + axl-cpp.pc |

AxlStream chosen as the first wrapper because it stresses the
design pattern (factory + sticky-error + method dispatch + maybe
range-for over bytes + maybe `std::string_view` overloads) before
applying it to four more types.

### Phase CPP1.7b — AxlEvent + AxlCancellable (deferred)

Concurrency trio with Stream.  Small wrappers (~50-60 LOC each).
Mostly mechanical application of the CPP1.7a pattern.

### Phase CPP1.7c — AxlStrBuf + AxlArena (deferred)

Memory-related wrappers.  Also mechanical.

### Phase CPP2 — Containers + iteration (deferred)

Per original ROADMAP: AxlArray, AxlList, AxlSlist, AxlQueue,
AxlHashTable wrappers with range-for support.  AxlSidecar /
AxlPci / AxlUsb cursor adapters.  `std::string_view` + `std::span`
overloads across the CPP1.7 surface.

### Phase CPP3 — Networking + format (deferred)

AxlHttpServer / AxlHttpClient wrappers.  Format-API revisited (if
a consumer asks; otherwise C printf family stays the answer).

### Phase CPP4 — Polish (deferred)

API hygiene pass, doc comments, examples sweep.

## Open questions parked for later

These are questions worth answering eventually but don't gate
implementation when it resumes.  Listed here so they don't get
lost.

- **Vendor-extension Handle promotion.**  Should `axlmm::detail::Handle`
  move to public `axlmm::Handle` to let vendor extensions wrap their
  own C handles?  Defer until a real vendor extension asks.

- **`std::format` viability.**  Run a CPP1.5-style validation pass
  to determine the exact unresolved-symbol surface in our
  freestanding link.  Decide if `<format>` is recoverable with
  stubs.  Defer until a consumer asks for type-safe format.

- **Per-tier ratchet attribution.**  When axlmm tests grow enough
  that "did C side regress vs C++ side" becomes a useful daily
  question (not just a `grep`), split `.last-pass-count` into per-
  tier files.

- **Inline impl vs split impl.**  When wrappers get heavy enough
  that header-only compile time matters, move individual functions
  to `src/axlmm/X.cpp` and let them join `libaxl-cxx.a`.

- **C++26 features.**  Reflection, `std::generator`, executors —
  revisit subset coverage when GCC ships them in stable releases
  the ARM bare-metal toolchain picks up.

## Related work

- [`AGT-Design.md`](https://github.com/aximcode/agt/blob/main/docs/AGT-Design.md) — the eventual primary axlmm
  consumer; axlmm's design lives or dies by whether AGT's actual
  patterns match the shapes documented here
- [`AXL-Display-Design.md`](AXL-Display-Design.md) — independent
  network display protocol track; may grow a C++ client lib later
  (`libaxl-display`); would also be an axlmm consumer if so
- [ROADMAP.md §"C++ Bindings — axlmm (Future)"](ROADMAP.md) — the
  phase tracker; points back to this doc
- `feedback_no_shortcuts` (auto-memory) — pre-1.0 reshapes of
  public API are allowed if they make the shape correct; applies
  if AGT validation surfaces a design issue here
