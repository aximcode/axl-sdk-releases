UEFI HII setup-form reader via the HII database + IFR opcode walk.

Header: `<axl/axl-hii.h>`. The platform's BIOS Setup menus are published
as HII form sets — a tree of IFR (Internal Forms Representation) opcodes
in the HII database, backed by EFI variables or driver-private config
storage. This module locates the HII protocols
(`EFI_HII_DATABASE/STRING/CONFIG_ROUTING/CONFIG_ACCESS`), exports every
form package once, walks the IFR stream into a cached typed model, and
projects each setting as an `AxlHiiQuestion`.

A consumer (e.g. a remote BIOS-setup UI) enumerates form sets, reads a
question's current value, and writes a new one without ever touching the
raw HII protocols or decoding IFR.

```c
if (axl_hii_available()) {
    size_t n = axl_hii_formset_count();
    for (size_t f = 0; f < n; f++) {
        char title[128], help[128];
        size_t qcount;
        if (axl_hii_formset_get(f, title, sizeof title,
                                help, sizeof help, NULL, &qcount) != AXL_OK) {
            continue;
        }
        for (size_t q = 0; q < qcount; q++) {
            AxlHiiQuestion question;
            uint64_t value;
            if (axl_hii_question_get(f, q, &question) == AXL_OK &&
                axl_hii_question_read(f, q, &value) == AXL_OK) {
                axl_printf("%a = %lu\n", question.prompt, value);
            }
        }
    }
}
```

### Scope

`ONE_OF`, `CHECKBOX`, `NUMERIC`, and `STRING` questions. STRING questions
are enumerated (prompt/help/min/max size); their text values go through
the dedicated `axl_hii_question_read_string` / `_write_string` pair, not
the u64 `read`/`write` (which return `AXL_ERR` for STRING). Forms,
cross-references, suppression/grayout expressions, and default stores are
out of scope: this is a flat list of the answerable questions in each
form set, not a form-browser.

### Value I/O

`axl_hii_question_read` resolves the question's variable store and reads
the value at its offset — first via `GetVariable` (EFI-backed stores),
falling back to the HII config-routing / config-access path
(`ExtractConfig` -> `ConfigToBlock`) for driver-private block stores.
`axl_hii_question_write` is the inverse (`GetVariable`/patch/`SetVariable`,
or `BlockToConfig` -> `RouteConfig`). Writes are guarded by the question's
read-only flag; the caller is responsible for passing a firmware-valid
value (a real ONE_OF option, or an in-range NUMERIC) — the module does
not range-check against the question's constraints.

STRING questions use `axl_hii_question_read_string` / `_write_string`,
which read/write the `CHAR16` text field at the question's offset (sized
by `max_size`) and convert to/from UTF-8. `_write_string` rejects input
longer than `max_size` and honors the read-only flag. (Every STRING
question stock OVMF/AAVMF publish is read-only, so the write path is
exercised on real hardware rather than in QEMU.)

### Lifecycle

The model is parsed lazily on the first `axl_hii_*` call and cached for
the life of the process (freed via an internal `axl_atexit` handler).
Values are read live on each `axl_hii_question_read`, so they reflect
writes made by this or any other agent.
