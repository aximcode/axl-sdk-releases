# AXL EFI Encapsulation Plan

**Status:** design — pre-Phase-A as of 2026-05-12. Phase C **shipped**
(`<axl/axl-fs-provider.h>`).
**Trigger:** session-end audit (post-v0.17.1) of axl-sdk consumers
for direct `EFI_*` / `gBS` / `gRT` / `EFIAPI` references.
**Goal:** consumers of axl-sdk write zero `EFI_*` identifiers and
never `#include <uefi/...>` — even when authoring drivers,
publishing protocols, or implementing UEFI spec interfaces.

> **Driver Model carried forward.** This plan's "authoring drivers"
> goal was only partially built: Phase C's `axl-fs-provider`
> (consumer-vtable → SDK EFIAPI-thunk publishing) covers the filesystem
> case, but the UEFI **Driver Model** (`EFI_DRIVER_BINDING_PROTOCOL` —
> `Supported`/`Start`/`Stop`) was never abstracted. That remaining piece
> is designed in [`AXL-Driver-Authoring-Design.md`](AXL-Driver-Authoring-Design.md),
> which reuses Phase C's thunk pattern for driver binding and adds the
> public `axl_protocol_install` over Phase P2's `axl_backend_install_protocol`.

## Design principle

axl-sdk's public surface is UTF-8 / `snake_case` / standard C
types ([CLAUDE.md](../CLAUDE.md)). UEFI's native ABI is UCS-2 /
`PascalCase` / `EFI_*` types. **The translation belongs in the
SDK, not in each consumer.** Today the SDK pulls this off cleanly
for application code (`<axl.h>` is enough) but leaks at three
fault lines: NVRAM access, boot-volume discovery / image
introspection, CPU exception handling, and — the largest — UEFI
spec-protocol *publishing* (filesystem providers, custom block
devices, vendor protocols).

This plan closes the leak in three phases, ordered by size and
risk. Phase A uses primitives the SDK already ships but consumers
didn't know to reach for. Phase B is a bounded new abstraction
(CPU exceptions). Phase C is real engineering — a filesystem
provider abstraction designed for downstream `axl-webfs`'s use
case but extensible to future FS-shaped consumers.

## Empirical baseline

Per the 2026-05-12 audit (axl-sdk v0.17.1):

| Consumer | Direct EFI hits | Where | Tier |
|---|---|---|---|
| `uefi-devkit/crashhandler/entrypoint.c` | 1 (comment only) | post-Phase-0 migration to `AXL_DRIVER` | clean |
| `uefi-devkit/crashhandler/exception.c` | 24 | exception-handler ABI parameters | Phase B |
| `uefi-devkit/crashhandler/report.c` | 42 | NVRAM + loaded-image walk + file writes | Phase A |
| `uefi-devkit/crashtest/crashtest.c` | 0 | post-Phase-0 | clean |
| `axl-webfs/src/mount/webfs-file.c` | 117 | `EFI_FILE_PROTOCOL` vtable | Phase C |
| `axl-webfs/src/mount/webfs-mount.c` | 14 | `EFI_SIMPLE_FILE_SYSTEM_PROTOCOL` publish + vendor device path | Phase C |
| `axl-webfs/src/mount/webfs-internal.h` | 14 | EFIAPI thunk prototypes + embedded protocol structs | Phase C |
| (a downstream diagnostics consumer's utils tree) | 0 (clean) | — | already clean |

Phase 0 (the `AXL_DRIVER` + `AxlHandle` migration in commit
`2186f5a` on uefi-devkit) is shipped. Everything below is forward
work.

---

## Phase A — use what the SDK already ships

**Effort:** ~1 day total. ~30 LOC of new SDK API + ~80 LOC removed
across the consumer (uefi-devkit crashhandler). Zero new SDK
abstractions invented.

**Deliverables:**
1. axl-sdk: `axl_app_boot_open` (or `_boot_volume_label`) and
   `axl_image_get_base` public helpers + unit tests.
2. **uefi-devkit consumer migration**:
   - `crashhandler/report.c` — `gRT->{Get,Set}Variable` →
     `axl_nvstore_*`; `EFI_FILE_PROTOCOL` open/write/close →
     `axl_file_set_contents`; the entire `open_boot_volume` static
     helper deleted in favor of `axl_app_boot_open`.
   - `crashhandler/exception.c` — the image-base device-path walk
     in the fault-attribution path collapses to
     `axl_image_get_base` + `axl_image_for_each`.
   - `crashhandler/crashhandler.h` — `process_crash_records`
     signature drops the image-handle parameter (helper looks it
     up internally via `axl_app_*`).
3. Validation: build clean both arches, run crashtest in QEMU,
   confirm a triggered #GP still writes crash-report.txt to the
   boot volume.

Both halves ship in a single SDK release because they're
co-dependent — the API can land first, consumer migration follows
within the same release window. Splitting buys nothing.

### A.1 — migrate crashhandler `report.c` to `axl_nvstore_*`

The crash-record persistence in `report.c` uses
`gRT->GetVariable` / `gRT->SetVariable` / `gRT->QueryVariableInfo`
directly. `<axl/axl-nvstore.h>` has covered this since v0.7.x —
nobody migrated.

```c
// before
status = gRT->GetVariable(CRASH_DUMP_IDX_VAR,
                          &g_crash_handler_variable_guid,
                          NULL, &slot_size, &slot_idx);

// after
axl_nvstore_register_namespace("crashdump", &g_crash_handler_namespace);
size_t got;
axl_nvstore_get("crashdump", "idx", &slot_idx, sizeof(slot_idx), &got);
```

Namespace registration replaces the per-variable GUID plumbing.
The existing `g_crash_handler_variable_guid` becomes the
namespace's identifying GUID, hidden inside one
`axl_nvstore_register_namespace` call at driver init.

### A.2 — migrate crashhandler `report.c` to `axl_fs_*`

`crash-report.txt` is written via `EFI_FILE_PROTOCOL` open / write
/ close. `<axl/axl-fs.h>` ships `axl_file_set_contents(path, buf,
size)` — one call, handles all the open / write / flush / close
plumbing.

### A.3 — new helper `axl_app_open_boot_volume_root`

The "find the FS where my image was loaded from" walk
(`gBS->HandleProtocol(image, EFI_LOADED_IMAGE_PROTOCOL) →
gBS->HandleProtocol(loaded_image->DeviceHandle,
EFI_SIMPLE_FILE_SYSTEM_PROTOCOL) → OpenVolume`) is duplicated
across mkrd, axl-webfs, crashhandler, and probably others.
Internalize:

```c
// <axl/axl-app.h>
/**
 * @brief Get the volume label of the filesystem the current
 *     image was loaded from.
 *
 * Convenience for tools that want to write output / read config
 * alongside their .efi without parsing axl_app_image_path().
 *
 * @return AXL_OK on success, AXL_ERR if the image source isn't
 *     a filesystem (boot-from-network, RAM disk, etc.).
 */
int
axl_app_boot_volume_label(
    char  *out,        ///< receives the volume label
    size_t out_size    ///< buffer size; 32 bytes is plenty
);
```

Or, more directly useful for the crashhandler case:

```c
/// Open a path on the volume the image was loaded from.
/// Equivalent to prefixing axl_app_image_path()'s volume to @p path.
int
axl_app_boot_open(
    const char *relative_path,  ///< path relative to boot volume root
    int         mode,           ///< AXL_FS_READ / _WRITE / etc
    AxlFile   **out_file
);
```

Both shapes solve crashhandler/report.c. Pick whichever fits the
broader consumer set after a brief callers-survey.

### A.4 — new helper `axl_image_get_base`

`crashhandler/exception.c` walks `EFI_LOADED_IMAGE_PROTOCOL.{ImageBase,
ImageSize}` for every loaded image to attribute a fault address
to a specific module. Surface:

```c
// <axl/axl-image.h>
int
axl_image_get_base(
    AxlHandle image,
    void    **out_base,   ///< [out] image base load address
    size_t   *out_size    ///< [out] image size in bytes
);
```

Combined with `axl_image_for_each` (likely already exists for the
enumeration), this drops `exception.c`'s entire device-path-walk
section.

### After Phase A

- `crashhandler/report.c`: ~42 hits → ~0
- `crashhandler/exception.c`: ~24 hits → ~4 (just the
  `EFI_EXCEPTION_TYPE` / `EFI_SYSTEM_CONTEXT` callback signatures,
  which Phase B handles)
- `crashhandler/entrypoint.c:99-100`: still 2 hits
  (`g_cpu->RegisterInterruptHandler`) — Phase B
- axl-webfs: no change (Phase C territory)

---

## Phase B — `<axl/axl-cpu.h>`: typed CPU exception abstraction

**Effort:** ~1-2 days SDK work + ~half-day consumer migration.
~200 LOC of SDK + per-arch context translation + 1 unit test
fixture + uefi-devkit crashhandler migration.
**Scope:** new public header, internal implementation atop
`EFI_CPU_ARCH_PROTOCOL`.

**Deliverables:**
1. axl-sdk: `<axl/axl-cpu.h>` with `axl_cpu_register_exception` /
   `_unregister_exception` + `AxlCpuException` typed context.
   QEMU-based per-arch test pinning the trigger-and-catch path.
2. **uefi-devkit consumer migration:**
   - `crashhandler/entrypoint.c` — the
     `axl_protocol_find_guid(gEfiCpuArchProtocolGuid, ...)` lookup
     and the `g_cpu->RegisterInterruptHandler` loop replaced by a
     single `axl_cpu_register_exception` loop over an
     `AxlCpuExceptionKind` array. `EFI_CPU_ARCH_PROTOCOL *g_cpu`
     global deleted.
   - `crashhandler/exception.c` — `crash_exception_handler`
     signature changes from
     `(EFI_EXCEPTION_TYPE, EFI_SYSTEM_CONTEXT)` to
     `(const AxlCpuException *, void *)`. The register-snapshot
     body rewrites against `exc->regs` instead of
     `system_context.SystemContextX64->Rip` etc. — the typed union
     is intentionally close in shape so the rewrite is mechanical.
   - `crashhandler/crashhandler.h` — `EFI_CPU_ARCH_PROTOCOL *g_cpu`
     extern + `EFI_EXCEPTION_TYPE` / `EFI_SYSTEM_CONTEXT` in the
     handler prototype gone.
3. Validation: build clean both arches; crashtest's #GP / #UD /
   #DE / #PF / data-abort modes all still write correct
   crash-report.txt entries.

### Public API

```c
// <axl/axl-cpu.h>

typedef enum {
    AXL_CPU_EXCEPTION_DIVIDE_ERROR,      ///< #DE  (x64) — divide by zero / overflow
    AXL_CPU_EXCEPTION_DEBUG,             ///< #DB  (x64)
    AXL_CPU_EXCEPTION_INVALID_OPCODE,    ///< #UD  (x64) / undefined-instruction (aa64)
    AXL_CPU_EXCEPTION_GP_FAULT,          ///< #GP  (x64) / sync-exception data-abort (aa64)
    AXL_CPU_EXCEPTION_PAGE_FAULT,        ///< #PF  (x64) / sync-exception MMU fault (aa64)
    AXL_CPU_EXCEPTION_ALIGNMENT_CHECK,   ///< #AC  (x64) / sync alignment-fault (aa64)
    AXL_CPU_EXCEPTION_DOUBLE_FAULT,      ///< #DF  (x64) — N/A on aa64
    AXL_CPU_EXCEPTION_SYNCHRONOUS,       ///< aa64 sync catch-all — N/A on x64
    AXL_CPU_EXCEPTION_SERROR,            ///< aa64 SError — N/A on x64
    /* ... full list TBD; mapped from EXCEPT_X64_* / EXCEPT_AARCH64_* */
} AxlCpuExceptionKind;

/// Architecture-neutral CPU exception context. Layout-stable
/// across SDK versions; arch-specific register data lives in the
/// tagged union below.
typedef struct {
    AxlCpuExceptionKind kind;
    uint64_t            fault_address;   ///< CR2 / FAR_EL1
    uint64_t            instruction_ptr; ///< RIP / ELR_EL1
    uint64_t            stack_ptr;       ///< RSP / SP
    uint64_t            frame_ptr;       ///< RBP / X29
    uint32_t            error_code;      ///< exception-specific (x64 #PF / #GP error code)
    enum { AXL_CPU_ARCH_X64, AXL_CPU_ARCH_AA64 } arch;
    union {
        struct { /* x64 register snapshot */ uint64_t rax, rbx, rcx, rdx, rsi, rdi, r8, r9, r10, r11, r12, r13, r14, r15, rflags; } x64;
        struct { /* aa64 register snapshot */ uint64_t x[31]; uint64_t spsr; } aa64;
    } regs;
} AxlCpuException;

typedef void (*AxlCpuExceptionHandler)(
    const AxlCpuException *exc,
    void                  *user
);

/**
 * Register a CPU exception handler. Internally locates the
 * EFI_CPU_ARCH_PROTOCOL and calls RegisterInterruptHandler with a
 * thunk that translates EFI_SYSTEM_CONTEXT → AxlCpuException
 * before calling @p cb. Idempotent on (kind, cb) pair.
 *
 * Conditional availability: on firmwares that don't publish
 * EFI_CPU_ARCH_PROTOCOL (rare but legal), returns AXL_ERR with a
 * warning to log domain "cpu". Consumers handle the "not
 * available" case explicitly rather than silently going
 * unmonitored.
 */
int
axl_cpu_register_exception(
    AxlCpuExceptionKind     kind,
    AxlCpuExceptionHandler  cb,
    void                   *user
);

/// Unregister a previously installed handler.
int
axl_cpu_unregister_exception(
    AxlCpuExceptionKind kind
);
```

### Internal implementation

- One translation table per arch mapping
  `AxlCpuExceptionKind` ↔ `EXCEPT_X64_*` / `EXCEPT_AARCH64_*`.
- One thunk function (per arch) — receives `EFI_EXCEPTION_TYPE` +
  `EFI_SYSTEM_CONTEXT`, fills an `AxlCpuException` from the
  union's relevant `EFI_SYSTEM_CONTEXT_{X64,IA32,ARM,AARCH64}`
  arm, calls the consumer callback with `user`.
- `axl_cpu_register_exception` lazily locates
  `EFI_CPU_ARCH_PROTOCOL` (once per image) and caches it.
- Internal state: small per-kind callback table keyed by
  `AxlCpuExceptionKind`.

### Test plan

QEMU + a test EFI that:
1. Registers a handler for `AXL_CPU_EXCEPTION_GP_FAULT`
2. Triggers an inline `int $13` or equivalent
3. Inside the callback: asserts `exc->kind`, `exc->arch`,
   `exc->fault_address` matches the deliberate fault
4. Sets a `g_caught` global, longjmps out
5. Test runner asserts `g_caught` was set and the test process
   continued (handler returned)

Per-arch parallel test. Existing `uefi-devkit/crashtest/`
provides the trigger-an-exception infrastructure to copy.

### After Phase B

- `crashhandler/entrypoint.c:99-100`: replaced by
  `axl_cpu_register_exception` loop
- `crashhandler/exception.c`: zero `EFI_*` references
- `crashhandler.h`: `g_cpu` and `EFI_EXCEPTION_TYPE` / `EFI_SYSTEM_CONTEXT`
  externs deleted
- crashhandler total EFI hits: **0**

---

## Phase C — `<axl/axl-fs-provider.h>`: filesystem-publisher abstraction

**Effort:** ~1 week SDK work + ~2-3 days axl-webfs migration.
~500-700 LOC of SDK + thorough tests + a co-designed axl-webfs
mount/ rewrite. **Largest item; ship as v0.19.x.**
**Scope:** new public header, internal `EFI_FILE_PROTOCOL` +
`EFI_SIMPLE_FILE_SYSTEM_PROTOCOL` thunk generator, vendor-device-path
helper, UTF-8 / UCS-2 boundary marshalling.

**Deliverables:**
1. axl-sdk: `<axl/axl-fs-provider.h>` + thunk implementation +
   `<axl/axl-device-path.h>` (surfacing the vendor-path helper) +
   in-tree mock-FS test + at least one example
   (`sdk/examples/memfs.c`?) that publishes a read-only RAM-disk
   FS via the provider API.
2. **axl-webfs consumer migration:**
   - `src/mount/webfs-internal.h` — `EFI_FILE_PROTOCOL` /
     `EFI_SIMPLE_FILE_SYSTEM_PROTOCOL` / `EFI_DEVICE_PATH_PROTOCOL`
     embedded structs deleted; the per-file state holds a
     `AxlFsProviderFile` instead. EFIAPI thunk prototypes deleted
     (the SDK emits the thunks now).
   - `src/mount/webfs-file.c` — 12 EFIAPI thunks
     (`WebFsOpen`/`Close`/`Read`/`Write`/`SetPosition`/
     `GetPosition`/`Delete`/`GetInfo`/`SetInfo`/`Flush`) collapse
     to ~9 axl-typed callbacks matching `AxlFsProvider`'s vtable.
     UCS-2 conversion code deleted (SDK boundary handles it). The
     `EFI_FILE_INFO` builder deleted (SDK marshals from
     `AxlFsProviderInfo`).
   - `src/mount/webfs-mount.c` — manual `EFI_DEVICE_PATH_PROTOCOL`
     construction (`HW_VENDOR_DP` + END terminator) replaced by
     `axl_device_path_make_vendor`. Manual protocol install
     replaced by `axl_fs_provider_publish`.
   - `src/mount/webfs-cache.c`, `webfs-protocol.h`, `cmd-mount.c`
     — comment-level updates only; no logic change.
3. Validation: existing axl-webfs integration tests (mount, ls,
   stat, read, write through Shell against an HTTP server backend)
   pass against the rewritten mount/. Add a regression test for
   the UCS-2/UTF-8 boundary: file with non-ASCII name (e.g.
   "résumé.txt", "日本語.bin") round-trips correctly through
   Shell `dir` and through `axl-webfs-mount` read/write.

axl-webfs's commit lands once axl-sdk's Phase C release is
tagged; the consumer migration bumps its axl-sdk pin in the same
commit.

### Why this needs a real abstraction

axl-webfs's `src/mount/` is a tier-1 *UEFI spec interface
publisher* — Shell, Boot Manager, and any other consumer expect to
`OpenProtocol(handle, &gEfiSimpleFileSystemProtocolGuid, ...)` and
get back a vtable with the exact EFI ABI. Hiding `EFI_FILE_PROTOCOL`
from the consumer means axl-sdk has to *be* the vtable on the
consumer's behalf — synthesize the function pointers, marshal
arguments, translate strings, propagate status codes.

### Provider vtable (UTF-8, snake_case, plain C types)

The shape below is the post-kickoff design (2026-05-12). It
diverges from the original sketch on nine concrete points called
out inline; each delta is a "no-shortcuts" call per
[[feedback-no-shortcuts]] — chosen for fidelity to UEFI semantics
and for the second-consumer cost rather than the immediate
single-consumer one.

```c
// <axl/axl-fs-provider.h>

/// Caller-supplied callback signatures. All paths are UTF-8.
typedef struct AxlFsProviderFile AxlFsProviderFile;

/// Typed status returned by every provider callback. Mapped to
/// EFI_STATUS once, by the SDK thunks. Lets providers preserve
/// semantic fidelity (Shell distinguishes NOT_FOUND from
/// DEVICE_ERROR from WRITE_PROTECTED from VOLUME_CORRUPTED) without
/// every callback growing an EFI_STATUS return type.
///
/// **Delta from sketch (#1):** original used plain `int` /
/// AXL_OK/AXL_ERR. Collapsing all failures to "ERR" left the thunk
/// inventing reasons; UEFI consumers care about the difference.
typedef enum {
    AXL_FS_OK = 0,
    AXL_FS_ERR_NOT_FOUND,
    AXL_FS_ERR_ACCESS_DENIED,
    AXL_FS_ERR_WRITE_PROTECTED,
    AXL_FS_ERR_NO_SPACE,
    AXL_FS_ERR_NOT_DIR,
    AXL_FS_ERR_IS_DIR,
    AXL_FS_ERR_INVALID,
    AXL_FS_ERR_NO_MEMORY,
    AXL_FS_ERR_IO,
    AXL_FS_ERR_UNSUPPORTED,
    AXL_FS_ERR_END_OF_FILE,
    AXL_FS_ERR_VOLUME_CORRUPTED,
} AxlFsStatus;

/// File-info attributes (mirrors EFI_FILE_DIRECTORY / _READ_ONLY
/// / _HIDDEN / _SYSTEM / _ARCHIVE bits in axl shape).
#define AXL_FS_ATTR_READ_ONLY  0x01u
#define AXL_FS_ATTR_HIDDEN     0x02u
#define AXL_FS_ATTR_SYSTEM     0x04u
#define AXL_FS_ATTR_DIRECTORY  0x10u
#define AXL_FS_ATTR_ARCHIVE    0x20u

/// Open-mode bitmask. Bit-compatible by intent with EFI_FILE_MODE_*
/// but providers see the AXL spelling and never the EFI one.
#define AXL_FS_OPEN_READ    0x1u
#define AXL_FS_OPEN_WRITE   0x2u
#define AXL_FS_OPEN_CREATE  0x4u

#define AXL_FS_PROVIDER_VERSION  1

/**
 * @brief Open a path. Provider sees the absolute UTF-8 path; the
 *     thunk has already resolved relative paths, "." / "..", and
 *     UCS-2 → UTF-8 conversion.
 *
 * **Delta from sketch (#3):** added `out_is_dir`. Lets the thunk
 * dispatch Read → read or read_dir without an extra stat round
 * trip. The provider already knows from looking up the entry.
 *
 * **Delta from sketch (#4):** path is always absolute UTF-8 with
 * forward slashes only. Thunk owns relative-path resolution,
 * `\` ↔ `/` conversion, and "." / "" → reopen-self semantics.
 * Providers never have to reimplement them.
 */
typedef AxlFsStatus (*AxlFsProviderOpen)(
    void               *backend_ctx,
    const char         *utf8_path,    ///< absolute UTF-8, '/' separators
    unsigned            mode,         ///< AXL_FS_OPEN_* bitmask
    unsigned            attributes,   ///< AXL_FS_ATTR_* (CREATE-only); DIRECTORY → mkdir
    AxlFsProviderFile **out,
    bool               *out_is_dir
);

typedef AxlFsStatus (*AxlFsProviderRead)(
    AxlFsProviderFile *file,
    void              *buf,
    size_t            *inout_size    ///< [in] req / [out] read; 0 = EOF
);

/**
 * @brief Read one directory entry. End-of-directory signaled by
 *     `*out_end = true` with status AXL_FS_OK.
 *
 * **Delta from sketch (#2):** original "reuse read with a flag"
 * conflated byte-mode and entry-mode buffers in one signature. The
 * separate callback keeps types honest and matches the existing
 * `<axl/axl-fs.h>` split.
 */
typedef AxlFsStatus (*AxlFsProviderReadDir)(
    AxlFsProviderFile *file,
    AxlFsProviderInfo *out,
    bool              *out_end
);

typedef AxlFsStatus (*AxlFsProviderWrite)(
    AxlFsProviderFile *file,
    const void        *buf,
    size_t            *inout_size
);

typedef AxlFsStatus (*AxlFsProviderSeek)(
    AxlFsProviderFile *file,
    uint64_t           position      ///< (uint64_t)-1 = EOF
);

typedef AxlFsStatus (*AxlFsProviderClose)(AxlFsProviderFile *file);

/// Delete the file referenced by @p file. The thunk calls @c close
/// after delete (regardless of outcome) per UEFI Delete spec.
typedef AxlFsStatus (*AxlFsProviderDelete)(AxlFsProviderFile *file);

typedef AxlFsStatus (*AxlFsProviderFlush)(AxlFsProviderFile *file);

/// Per-file metadata. struct_size + version prefix lets the SDK
/// grow the struct without breaking existing providers
/// (consumers/providers test `info.struct_size >= offsetof(...,
/// new_field) + sizeof(new_field)`).
///
/// **Delta from sketch (#9):** added struct_size + version prefix
/// to mirror AxlCpuException.
typedef struct {
    uint32_t struct_size;     ///< sizeof(AxlFsProviderInfo) at write time
    uint32_t version;         ///< AXL_FS_PROVIDER_VERSION
    char     name[256];       ///< UTF-8, null-terminated, basename only
    uint64_t size;
    uint64_t mtime_unix;
    uint32_t attributes;      ///< AXL_FS_ATTR_* bitmask
} AxlFsProviderInfo;

typedef AxlFsStatus (*AxlFsProviderGetInfo)(
    AxlFsProviderFile *file,
    AxlFsProviderInfo *out
);

typedef AxlFsStatus (*AxlFsProviderSetInfo)(
    AxlFsProviderFile       *file,
    const AxlFsProviderInfo *in
);

/// Volume-level info reported by GetInfo on
/// gEfiFileSystemInfoGuid. struct_size + version prefix per #9.
typedef struct {
    uint32_t struct_size;
    uint32_t version;
    bool     read_only;
    uint64_t volume_size;     ///< (uint64_t)-1 if unknown
    uint64_t free_space;      ///< (uint64_t)-1 if unknown
    uint32_t block_size;
    char     label[64];       ///< UTF-8 volume label
} AxlFsProviderVolumeInfo;

/**
 * @brief Optional volume-info callback.
 *
 * **Delta from sketch (#6):** EFI_FILE_SYSTEM_INFO and
 * EFI_FILE_SYSTEM_VOLUME_LABEL are top-level GetInfo GUIDs, not
 * per-file. If NULL, the thunk synthesizes a default ((uint64_t)-1
 * sizes + the static label from `AxlFsProvider.default_label`).
 */
typedef AxlFsStatus (*AxlFsProviderVolumeInfoFn)(
    void                    *backend_ctx,
    AxlFsProviderVolumeInfo *out
);

/// Provider vtable. struct_size + version prefix per #9; the SDK
/// reads only the fields its own version recognizes, so providers
/// built against newer SDKs add fields at the tail without breaking
/// older SDKs that consume them.
typedef struct {
    uint32_t struct_size;     ///< sizeof(AxlFsProvider) at init time
    uint32_t version;         ///< AXL_FS_PROVIDER_VERSION

    AxlFsProviderOpen         open;
    AxlFsProviderClose        close;
    AxlFsProviderRead         read;
    AxlFsProviderReadDir      read_dir;     ///< NULL → directory open returns ERR_UNSUPPORTED
    AxlFsProviderWrite        write;        ///< NULL → all writes return WRITE_PROTECTED
    AxlFsProviderSeek         seek;
    AxlFsProviderDelete       del;          ///< NULL → all deletes return WRITE_PROTECTED
    AxlFsProviderFlush        flush;        ///< NULL → flush is a no-op
    AxlFsProviderGetInfo      get_info;
    AxlFsProviderSetInfo      set_info;     ///< NULL → set_info returns WRITE_PROTECTED
    AxlFsProviderVolumeInfoFn volume_info;  ///< NULL → thunk synthesizes default
    const char               *default_label; ///< used when volume_info NULL; "" allowed

    void                     *backend_ctx;  ///< passed to open / volume_info
} AxlFsProvider;

/**
 * Publish a filesystem on a new UEFI handle. Internally creates a
 * vendor device-path (axl_device_path_make_vendor), synthesizes
 * EFI_SIMPLE_FILE_SYSTEM_PROTOCOL and EFI_FILE_PROTOCOL vtables
 * that forward into @p provider, installs both protocols on the
 * new handle, and returns an opaque handle the consumer can pass
 * to axl_fs_provider_unpublish.
 *
 * The consumer sees no EFI_* types. UEFI consumers (Shell, etc.)
 * see a perfectly-conformant filesystem they can `dir` / `ls` /
 * `cd` into.
 *
 * @p vendor_guid identifies the provider kind so multiple
 * instances are distinguishable in the device-path namespace.
 */
int
axl_fs_provider_publish(
    const AxlFsProvider *provider,
    const AxlGuid       *vendor_guid,
    void               **out_handle  ///< [out] for unpublish
);

/**
 * @brief Unpublish a previously-published filesystem.
 *
 * **Delta from sketch (#5):** force-closes every outstanding
 * AxlFsProviderFile (calling provider's `close` on each) before
 * uninstalling protocols. The original "refuse if files open"
 * idea was a non-starter for AXL_SERVICE_DRIVER teardown — the
 * driver image (and the vtable pointers in it) is going away.
 * UEFI consumers that hold a stale EFI_FILE_PROTOCOL pointer
 * after unpublish get EFI_DEVICE_ERROR on next call (the thunk
 * retains a small "dead" record per handle for a short window
 * so the deref doesn't fault).
 */
int
axl_fs_provider_unpublish(void *handle);
```

### The UCS-2 / UTF-8 boundary — design-critical

axl-sdk's `<axl/axl-str.h>` ships `axl_ucs2_to_utf8 /
axl_utf8_to_ucs2` (and length variants). They live at the
provider/EFI boundary inside `<axl/axl-fs-provider.h>`'s thunks:

- **Inbound (UEFI consumer calls into us):** EFI_FILE_PROTOCOL's
  `Open` receives a `CHAR16 *` path. The thunk converts to UTF-8
  into a stack/heap buffer, calls the provider's `open(utf8_path,
  ...)`, returns the result. Same conversion on `SetInfo`
  rename-via-FileName.
- **Outbound (we return data to UEFI):** `GetInfo` requires
  building an `EFI_FILE_INFO` struct whose tail carries the UCS-2
  filename. The thunk takes the provider's UTF-8 name from
  `AxlFsProviderInfo.name`, converts to UCS-2, lays out the
  EFI_FILE_INFO header + UCS-2 trailer in the
  consumer-supplied buffer (handling `EFI_BUFFER_TOO_SMALL`
  correctly).

Buffer-too-small is the trap path: EFI consumers call `GetInfo`
with a 0-length probe to learn the size, then re-call with the
right buffer. UCS-2 expansion of a UTF-8 name isn't 1:1
(`uint16_t` per code unit vs `uint8_t` per byte), so the thunk
must compute the converted size on the probe path.

**Delta from sketch (#7):** `axl_utf8_to_ucs2_buf` ships today as
a Latin-1 cast (`dst[i] = (unsigned char)src[i]`). It silently
corrupts non-ASCII names — "résumé.txt" round-trips wrong. The
non-ASCII regression test in the acceptance criteria can't pass
without fixing this helper, so a real UTF-8 decoder is in-scope
for Phase C (alongside a unit test pinning the multi-byte
encoder/decoder pair).

### Vendor device-path helper

```c
// <axl/axl-device-path.h>  (new — surfaces internal infra)

/// Opaque axl-side handle to an EFI_DEVICE_PATH_PROTOCOL chain.
/// The pointer returned in @p out is suitable to pass to
/// axl_protocol_register("device-path", ...).
typedef struct AxlDevicePath AxlDevicePath;

/**
 * @brief Allocate a vendor device-path node + END terminator with
 *     the given GUID.
 *
 * **Delta from sketch (#8):** allocator that returns a freshly
 * heap-allocated chain. Caller frees with `axl_free`. Mirrors how
 * webfs's mount/ and crashhandler's report.c each currently
 * hand-roll a malloc + template-memcpy + GUID-fill.
 */
int
axl_device_path_make_vendor(
    const AxlGuid  *vendor_guid,
    AxlDevicePath **out
);
```

axl-webfs's mount/ constructs a manual `EFI_DEVICE_PATH_PROTOCOL`
(`HARDWARE_DEVICE_PATH` + `HW_VENDOR_DP` with a GUID + a
`END_DEVICE_PATH_TYPE` terminator). Surface the existing internal
constructor.

### Test plan

QEMU-based test: an SDK test EFI publishes an in-memory mock
filesystem (10-file flat root with known content), then runs
`fs0:/list-known-files.efi` to verify the Shell-side enumeration
matches the consumer's expected layout. Two extension tests:
write-side (the test EFI publishes a write-back-cache provider,
runs `copy memfs:/a fs0:/b` through Shell, verifies the cache),
and dir-enum (>1000 entries to flush any fixed-buffer
assumptions).

### After Phase C

- axl-webfs `src/mount/` shrinks from ~12 EFIAPI thunks + manual
  UCS-2/EFI_FILE_INFO marshalling to ~9 axl-typed callbacks
- All `EFI_*` references in axl-webfs's `src/mount/` disappear
- The only `#include <uefi/...>` in axl-webfs is from axl-sdk's
  own umbrella — i.e., transitive, invisible to authors
- A second FS-provider consumer (HTTP-mirror as a read-only FS?
  compressed-bundle viewer? mock-fs for axl-webfs's own integration
  tests?) becomes a ~50-LOC project instead of ~1500

---

## Non-goals

- **Rewriting tier-1 UEFI spec headers.** The generated
  `<uefi/...>` headers stay; they're how axl-sdk *internally*
  talks to firmware. The non-goal is that consumers never
  `#include <uefi/...>` themselves.
- **Eliminating `axl_efi_call` macro / `axl_efi_status`
  primitives.** These exist as documented escape hatches for
  consumers who genuinely need to call an unwrapped UEFI service
  (rare future spec we haven't abstracted yet). Their existence is
  feature, not bug. The goal is that *no current consumer code
  path* needs them.
- **Hiding from advanced consumers.** A power user who wants
  direct UEFI control can still `#include <uefi/axl-uefi.h>` and
  call `gBS->...` — axl-sdk doesn't break that. It just stops
  *requiring* it.

---

## Sequencing

| Phase | Trigger | Target axl-sdk release | Consumer commit pair | Effort (SDK + consumer) |
|---|---|---|---|---|
| A | Crashhandler still has 60+ EFI hits | v0.18.0 | uefi-devkit crashhandler/report.c + exception.c image-base | ~1 day |
| B | Crashhandler Phase A still has 4 hits in exception.c | v0.18.0 (same release) or v0.18.1 | uefi-devkit crashhandler entrypoint + exception + header | ~1.5–2.5 days |
| C | axl-webfs `src/mount/` has 148 hits, no other consumer needs it yet | v0.19.0 | axl-webfs `src/mount/` rewrite | ~1.5 weeks (SDK ~1w + axl-webfs ~2-3d) |

A + B together get the public message right ("axl-sdk drivers can
write zero EFI"). C is for the FS-publisher class of consumers
which is rarer but where the duplication is highest. The full
plan is a six-week stretch goal; the visible-win 80% is two days
of work.

---

## Open questions

1. **`axl_app_boot_volume_label` vs `axl_app_boot_open`** (A.3) —
   pick after a callers-survey of `axl_app_image_path()` usage.
   Probably ship both; they cost the same.
2. **Where do `axl_image_get_base` and friends live?** —
   `<axl/axl-image.h>` is current candidate; alternative is a
   new `<axl/axl-process.h>` if we want a runtime-introspection
   home distinct from the load/start/unload primitives.
3. **Phase B exception-kind coverage** — keep the enum tight (the
   ~10 kinds crashhandler watches today) or pre-declare every
   `EXCEPT_X64_*` value? Tight enum is easier to expand later;
   exhaustive is one-shot but locks the surface. Recommend tight.
4. **Phase C provider lifetime** — RESOLVED at kickoff. Force-close
   semantics (delta #5 in Phase C); see body for details.
