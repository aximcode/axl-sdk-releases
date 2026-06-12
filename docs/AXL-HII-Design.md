# AXL HII / Forms-Engine Design

**Status:** DRAFT — living doc, for iteration. Opened 2026-06-11.
**Scope:** a headless UEFI HII (Human Interface Infrastructure) forms
engine in axl-sdk — `axl-hii` — that reads the HII database, parses IFR
into a forms/questions model, evaluates the IFR expression VM
(suppressif / grayoutif / disableif / inconsistentif, value + default
expressions), and reads/writes the question stores via config routing —
all **without a GUI**. The visual layer (mapping questions to widgets)
lives in AGT as `AgtFormBrowser` and is out of scope here.

Companion to [AXL-Dashboard-Server-Design.md](AXL-Dashboard-Server-Design.md):
both are applications of the same substrate-vs-application line, and they
**share a consumer** — see §6.

Nothing here is committed — it's the thing we iterate on before code.

---

## 0. The split, and why this layer is in axl-sdk

The deciding test is **"does it have to work without a GUI?"** — and HII
does: SoftBMC's Redfish BIOS-attributes, a headless `config dump`, and
scripting all need to read *and* write HII forms with no widgets. That
forces the **data + logic** layer down into axl-sdk; only the
**rendering** is AGT's.

This mirrors a seam the firmware world already proved: EDK2 splits
`SetupBrowserDxe` (forms logic) from `DisplayEngineDxe` (rendering) over a
protocol. We mirror that boundary:

| EDK2 | AXL | Home |
|---|---|---|
| `SetupBrowserDxe` (forms + expression logic) | **`axl-hii`** | axl-sdk (headless substrate) |
| `DisplayEngineDxe` (rendering) | **`AgtFormBrowser`** | AGT (composite widget) |

**The argument that actually clinches axl-sdk ownership is the expression
VM, not the table reads.** suppressif / grayoutif / disableif /
inconsistentif + value/default expressions are a stack-based bytecode
interpreter, and **headless validation needs it**: SoftBMC's Redfish must
reject an invalid BIOS-attribute set the *same way* the GUI would. So the
evaluator cannot live in AGT without either duplicating it or making
headless validation impossible. Everything else (IFR parse, string
resolve, config routing) is supporting cast around that core.

`axl-hii` slots into the firmware-introspection family next to
`axl-smbios` / `axl-acpi` / `axl-pci` / `axl-usb` / `axl-ipmi` / `axl-spd`
— but see §2: it is **a heavier module than those**, not a peer
table-parser.

---

## 1. What it is NOT (scope guards)

- **Not a renderer.** No widgets, no layout, no fonts (see below). The
  model is paradigm-agnostic; AGT and the web dashboard are two front-ends
  over it.
- **Not an AML interpreter** (that's ACPI, and explicitly out of scope in
  `axl-acpi` for the same reason — ACPICA-sized).
- **Not HII *font* packages.** A headless engine needs HII **string**
  packages (resolve string IDs → labels, multi-language). Font/glyph
  packages are rendering — AGT already owns a font stack (TTF/axl-gfx), so
  HII font packages are AGT's concern or out of scope. **Strings down,
  fonts up.**
- **Not the BIOS-setup app.** A shipped `AximSetup.efi` (links axl-sdk +
  AGT + the form browser) is a *product*, the same bucket as the axedit /
  hexview tool-apps and SoftBMC — out of scope for the library.

---

## 2. Reality check — this is a heavyweight module

"Slots straight into the axl-smbios/axl-acpi family" undersells it by a
weight class. SMBIOS/ACPI/PCI are read-mostly **table parsers**; `axl-hii`
is a **forms engine + an expression VM + a read/write config-routing
layer**:

- **IFR opcode parsing** — dozens of opcodes (form/formset, oneof+options,
  checkbox, numeric, string, password, orderedlist, ref, action, text,
  subtitle, nested scopes, the conditional opcodes, default/value
  expressions).
- **The expression VM** — HII expressions are stack-based bytecode
  (get/set, boolean, comparison, arithmetic, string ops) over the question
  store. A real interpreter.
- **String packages** — multi-language, UCS-2, string-ID resolution.
- **Config routing** — the `<ConfigRequest>` / `<ConfigResp>`
  (`OFFSET=…&WIDTH=…&VALUE=…` block-name *and* name/value formats), efivar
  storage, *and* driver-managed storage via
  `EFI_HII_CONFIG_ACCESS_PROTOCOL` callbacks.
- **Default stores** — standard / manufacturing defaults, default
  expressions.

This is plausibly the **single biggest module** proposed for axl-sdk. Plan
it as its own multi-phase sub-project (this doc), not "another parser." It
also adds real **audit/maintenance surface** (it *writes firmware
settings*) — the "no bloat cost" claim is true only for binary size
(selective linking); the security-surface cost is real, so build it
smallest-mechanism-first with the write path gated.

---

## 3. Design seams

### 3.1 Protocol-independent core (buffer-in), like axl-smbios
Do **not** weld the parser to `EFI_HII_DATABASE_PROTOCOL`. The engine
takes an **HII package buffer** (+ a question store), with a thin live-DB
adapter layered on top — exactly the `axl_smbios_table_range` precedent
(expose the raw table; parse the buffer). This is what makes the engine:
- **Unit-testable** — feed canned IFR + a store, assert the model, assert
  suppress/grayout under given store values, assert `inconsistentif` fires,
  assert a `ConfigResp` round-trips. Deterministic, QEMU-friendly, no GUI.
- **Fixture-replayable** — capture a real machine's HII packages (HF-style)
  and replay under QEMU, where OVMF's own setup forms are thin.

The headless ⇒ unit-testable property is itself a first-class reason the
logic belongs in axl-sdk (where the test discipline + ratchet live), and
it is how the expression VM gets de-risked.

### 3.2 The model
A paradigm-agnostic tree: formset → forms → questions, each question typed
(oneof/checkbox/numeric/string/password/orderedlist/ref/action/text) with
its options, prompt/help string IDs (resolved via §3.1 strings), current
value (from the store), default(s), and the conditional expressions
attached. Consumers (AGT, Redfish, scripts) walk this tree; they never see
raw IFR.

### 3.3 Read before write — phase the danger
The headless consumers cited (Redfish attribute *read*, `config dump`,
scripting reads) are mostly **read**. The danger concentrates in the
**write** path: config-routing back to stores, `ConfigAccess` callbacks,
re-running consistency on write — where you can brick a config or trip
`inconsistentif`. So phase it (see §5) with the write path landing last and
gated.

---

## 4. AGT side (out of scope here, sketched for the seam)

`AgtFormBrowser` is a composite widget (like `AgtFileDialog`) that consumes
the `axl-hii` model and:
- maps each question to a widget — oneof→combo/radio, checkbox→AgtCheckBox,
  numeric→AgtSpinBox/AgtSlider, string→AgtEditField,
  password→AgtPasswordField (nice synergy: BIOS passwords), orderedlist→a
  reorderable list, ref→subform navigation, action→AgtButton, text→AgtLabel;
- lays them out (scrolling AgtVBox);
- applies suppress/grayout to widget visibility/enabled **by asking the
  axl-hii evaluator** — the GUI never re-implements the expression logic;
- drives Save / Discard / Load-Defaults against the axl-hii write API.

It calls the axl-sdk C APIs directly (the house pattern). It can begin once
the model + expression VM (phases 1–2) land; it only needs the write API
(phase 4) for Save.

---

## 5. Phasing (proposed)

1. **Parse + model + string resolve** `[substrate]` — IFR → forms/questions
   tree, string-ID resolution, buffer-in core + live-DB adapter. Read-only.
   Unit-tested against canned IFR.
2. **Expression VM** `[substrate]` — the centerpiece. suppressif /
   grayoutif / disableif / inconsistentif + value/default expressions over
   the store. Heavily unit-tested (this is where bugs hide). After this,
   AGT can render + apply suppress/grayout.
3. **Config read / round-trip** `[substrate]` — read current values via
   config routing; `ConfigResp` parse + emit; dump current settings
   headlessly. Covers most of the Redfish *read* path.
4. **Write + default stores** `[substrate mechanism]` — config routing back
   to efivar + driver-managed stores (`ConfigAccess`), standard /
   manufacturing defaults, consistency re-check on write. **Gated**,
   smallest-mechanism, most scrutiny. Unblocks AGT Save and Redfish
   attribute *write*.
5. **AGT `AgtFormBrowser`** `[AGT]` — the renderer; starts after phase 2,
   completes after phase 4.
6. **(optional) `AximSetup.efi`** `[app]` — a shipped BIOS-setup-style tool;
   a product, not library work.

---

## 6. Tie-in with the dashboard server

Concrete, not just analogy: the dashboard's Tier-3 **Redfish
BIOS-attributes** resource (see
[AXL-Dashboard-Server-Design.md](AXL-Dashboard-Server-Design.md) §3.6) is a
**headless `axl-hii` consumer**. So `axl-hii` becomes shared substrate
under **two independent renderers**:

- AGT `AgtFormBrowser` — local GUI widgets;
- the web dashboard's Redfish/HTML endpoints — projected to the browser.

Two renderers over one headless core is the cleanest validation that the
seam is in the right place — and the expression VM + ConfigResp round-trip
get exercised by both paths, so the test investment pays double. (A Redfish
EventService could even surface HII changes over the dashboard's SSE
primitive — speculative, noted only as a downstream possibility.)

---

## 7. Open questions / to iterate

- **Store abstraction:** what's the question-store interface the engine
  reads/writes — an opaque `AxlHiiStore` with get/set-by-(guid,offset,width)
  + name/value, so efivar and `ConfigAccess`-backed stores plug in behind
  one vtable (mirrors the AxlSmbus transport vtable)?
- **Expression VM surface:** evaluate-on-demand (ask "is question Q
  suppressed given the current store?") vs. a precomputed pass? On-demand is
  simpler and matches both renderers' "tell me the state of Q now" needs.
- **Write safety:** does the engine enforce `inconsistentif` on write and
  refuse, or report and let the consumer decide? (Leaning: enforce + return
  the failing constraint, so Redfish and the GUI get identical rejection.)
- **Fixture capture:** add HII-package capture to the HF (hardware-fixture)
  track so real-machine forms replay under QEMU?
- **Scope re-confirm per phase:** the §0 line (strings down / fonts up,
  mechanism in axl-sdk / roles+rendering in the consumer) gets re-checked
  before each phase — same discipline as the dashboard doc.

---

## Appendix — relevant existing surface

- Protocol GUIDs already generated: `EFI_HII_DATABASE_PROTOCOL`,
  `EFI_HII_STRING_PROTOCOL`, `EFI_HII_CONFIG_ROUTING_PROTOCOL`,
  `EFI_HII_PACKAGE_LIST_PROTOCOL`, font/image protocol GUIDs — in
  `include/uefi/generated/guids.h`. No `axl-hii` code exists yet.
- Precedent for buffer-in + fixture replay: `axl-smbios.h`
  (`axl_smbios_table_range`) and the Hardware-Fixture track
  ([AXL-Hardware-Fixture-Design.md](AXL-Hardware-Fixture-Design.md)).
- Manifest gap to close: `scripts/uefi-manifest.json5` will need the HII
  IFR/package structs added (the ROADMAP's "add more protocols to the
  manifest — PCI, USB, HII" item; PCI/USB are done, HII is the remaining
  one).
