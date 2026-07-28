# axl-gfx — Local Compositor & Seat Design

**Status:** building — **C1–C6 landed** (surfaces + tree compositing +
present + the seat: pointer routing, grabs, keyboard focus + the cursor
overlay; C7 = AGT migration remains; see §8).  Captures the target
architecture so incremental moves (and the AGT toolkit above it) align.

A **local, in-process** compositor for axl-gfx: per-surface buffers,
z-ordered stacking, multi-surface present on the single GOP framebuffer,
and a **seat** (pointer grab + per-surface keyboard focus).  It lets a
toolkit (AGT today) stop faking window stacking with "reparent a child so
it paints last," and gives a clean, reusable windowing primitive for any
axl-sdk app that wants stacked surfaces + input without a widget set.

> **Not the same as [`AXL-Display-Design.md`](AXL-Display-Design.md).**
> That is the **remote** display protocol — client and server on
> different machines, so it is deliberately **X-shaped** (the
> client/server boundary is the network).  *This* doc is the **local**
> compositor — one process, shared memory — so it is deliberately
> **Wayland-shaped**.  Remote = X-shaped; local = Wayland-shaped.  They
> can coexist (the network server could drive local surfaces), but they
> are different layers.

> **Toolkit side:** the consumer's within-surface focus logic
> (tab traversal, focus policy, focus groups) does NOT live here — see
> the seam in §4 and AGT's
> [`AGT-Input-Focus-Design.md`](../../agt/docs/AGT-Input-Focus-Design.md).

---

## 1. The layering, and why it collapses to two layers here

The classic desktop stack is three layers across a process boundary:

```
X11 server  ──wire──  Xlib/xcb  ──  Xt/Motif/GTK/Qt        (multi-process)
```

The server and the toolkit are **different processes**, so the middle
layer exists to marshal a wire protocol between them.  AGT/axl-sdk run as
**one UEFI process**, which removes the boundary and collapses the middle
layer entirely:

```
axl-gfx compositor + seat  ──C API──  AGT (toolkit)          (one process)
        "the server"        (no IPC)     "the client"
```

- **No protocol / no Xlib-equivalent.**  The toolkit links the
  compositor and calls its C functions directly; nothing is serialised.
  This is the single biggest reason a Wayland-*shaped* compositor here is
  light where a real one is heavy.
- **No multi-client machinery.**  One app is foregrounded in pre-boot
  UEFI (same assumption as `AXL-Display-Design.md` §Non-goals).  So: no
  client isolation, no per-client security, no resource arbitration.
  "Surfaces" all belong to the one in-process client.
- **What's left is the genuinely useful core:** surfaces, z-order,
  compositing/present on one framebuffer, input routing, and a seat.

---

## 2. What the compositor owns

### 2.1 Surfaces — a tree, not a flat list
A **surface** is a CPU-backed rectangle the client draws into and asks
the compositor to present.  (Today this is `AxlGfxBuffer` + AGT's
`back_buf_`; a surface formalises it with position, visibility, and a
place in a tree.)

- Per-surface back-buffer (`AxlGfxBuffer`), origin, size, visible flag,
  opacity (dialog-veil/dim), `opaque` hint (§7), optional input region
  (§2.3), and a damage rect.
- Create / destroy / move / resize / raise / lower / show / hide.
- The client draws into a surface exactly as it draws into a buffer today
  (`axl_gfx_target_buffer(surface_buffer)` + the existing primitives).

**Surfaces form a tree (a scene graph), because stacking is inherently
hierarchical.**  This is Wayland's model — unifying its two parent
concepts (`wl_subsurface`'s relative-positioned child and `xdg_popup`'s
parent-anchored child) into **one**, with the multi-client parts dropped
(no sync/desync commit modes, no roles — there are no async or untrusted
clients here):

- Each surface has an optional **parent**; the compositor holds the tree
  with the screen as the implicit root.  A top-level window is a child of
  the root; a menu/tooltip/dialog is a child of the window it belongs to.
- **Child position is relative to the parent origin** — move a window and
  its whole subtree (menus, tooltips) follows for free.
- **Stacking = pre-order DFS, siblings in list order:** a surface paints,
  then each child subtree paints on top in order, recursively.  `raise(s)`
  moves `s` to the top of its sibling list and **the entire subtree
  travels as a unit** — raise window B above A and B *and B's popups* go
  above A and A's popups.  O(subtree), not O(all surfaces); a flat z-index
  would force the toolkit to recompute every z on each raise.
- **No clip to parent** — a menu may drop below its menubar (as Wayland
  subsurfaces/popups do).
- `show` / `hide` / `move` / `destroy` act on the **subtree**; destroying
  a surface destroys its descendants (closing a window kills its menus).

This is the DRY/SOC split made concrete: the compositor owns the **coarse
tree** (windows/popups — stacking, relative move, subtree lifecycle); the
toolkit owns the **fine tree** (widgets *within* a surface — §4).  AGT's
C++ window/popup hierarchy maps 1:1 onto the surface tree (each
`AgtWindow`/`AgtPopup` wraps one `AxlSurface`; the parent link mirrors C++
ownership), so AGT never reimplements stacking, relative positioning, or
popup-chain dismissal.

### 2.2 Compositing & present
The compositor composites visible surfaces **in tree pre-order (§2.1)** —
each surface then its children on top — into a single full-screen
**output** back-buffer it owns, honouring per-surface opacity, and
presents **only damaged regions** of the output to the GOP (extending
today's `axl_gfx_buffer_present_damage`), with the cursor as the top
overlay (§2.4).  So total buffers = one output buffer + the N
content-sized surface buffers + the cursor's small compose buffer.

> **Terminology** (vs Wayland — see §1.2): we call the composited result
> the **output** buffer (DRM reserves "scanout" for a buffer handed
> *directly* to a hardware plane).  **`present`** is the Vulkan / DXGI /
> X-Present sense — composite + put on screen — *and* doubles as Wayland's
> `wl_surface.commit` (the atomicity barrier), since one synchronous
> client means there is no separate pending-state commit to name.

- This is what kills AGT's "reparent the popup to be the last child so it
  paints on top" hack — popups/menus/tooltips/dialogs become real stacked
  surfaces with their own buffers, composited in order.
- It also fixes the class of bug behind the dialog-veil readback: a dim
  veil is a translucent surface composited over the surfaces beneath it,
  not a blit of a back-buffer the client has to read back.
- **`axl_compositor_present()` is the atomicity barrier.**  Wayland's
  defining property — "every frame is perfect," no tearing or
  half-updated frames — comes from double-buffering *every* surface's
  state (pending → atomic `wl_surface.commit`).  We get the same
  guarantee for free without that machinery: there is **one synchronous
  client**, so it mutates surfaces (move / raise / damage / draw) and
  *then* calls `present()`, which composites one coherent frame.  Nothing
  is shown mid-update because the client controls exactly when present
  runs.  So per-surface commit AND frame callbacks (Wayland's
  render-throttling handshake) collapse away — the same single-client
  collapse that removes the wire protocol (§1).  The surface setters are
  therefore immediate state writes, not double-buffered; present is the
  commit point.
- **Geometry changes damage old + new.**  Moving / lowering / hiding a
  surface must repaint what it *uncovered* (its previous bounds) as well
  as where it landed — composite damage for a geometry change is
  `union(old_bounds, new_bounds)`, not just the surface's own damage
  rect.  (Classic compositor footgun; easy to miss.)
- **Cost to keep honest:** N surface buffers cost N×W×H×4 bytes, and
  compositing is per-damaged-pixel work.  Pre-boot is memory-constrained,
  so: small surface counts (a handful of popups deep), damage-driven
  present, and opaque-surface occlusion culling (see §7 — a per-surface
  `opaque` flag is the cheap 80/20).  Document any cap.

### 2.3 Input routing + the seat
The compositor receives raw `axl-input` events and routes them.  A
**seat** (Wayland `wl_seat` shape) is the per-input-focus state:

- **Pointer:** hit-test surfaces top-to-bottom → deliver to the surface
  under the pointer (in surface-local coords).  A **pointer grab** (LIFO
  stack) overrides hit-test so all pointer events go to the grabbing
  surface — the `XGrabPointer`/menu mechanism.  A click **outside the
  grabbing surface's subtree** (§2.1) is the dismiss signal: it pops the
  grab and the toolkit closes the popup chain.
- **Keyboard:** a **per-surface keyboard focus** — the seat tracks which
  surface has the key focus; key events route there.  (This is the
  *surface*-level focus, NOT the within-surface widget focus.)
- Enter/leave + focus-in/out notifications to surfaces on transitions.
- **Re-hit-test after *any* surface change, not just on motion.**  A
  raise / lower / move / hide / show can put a different surface under a
  stationary pointer, which must emit synthetic leave-old + enter-new.
  Wayland does this; forgetting it leaves hover/focus stale after
  z-order changes.
- **Optional per-surface input region** (Wayland `set_input_region`).
  Hit-test defaults to the full surface rect, but a surface may mark part
  of itself input-transparent — load-bearing for the dim **veil** (does
  it swallow clicks or pass them through? — decide explicitly) and for
  shadowed / rounded popups whose margins shouldn't grab the pointer.
- **No serial-authentication on grabs.**  Wayland gates grab /
  set-cursor on input-event serials to stop a malicious client stealing
  focus; with **one trusted in-process client there is no threat model**,
  so we omit it.  (Noted so it isn't cargo-culted back in.)
- **Event delivery is a per-surface listener struct + `void *user`**
  (Wayland `wl_pointer_listener`/`wl_keyboard_listener` shape): one
  `AxlSurfaceListener` of function pointers — `enter`, `leave`, `motion`,
  `button`, `axis`, `key`, `focus_in`/`out`, all
  surface-local — set via `axl_surface_set_listener(s, &listener, user)`.
  The pointer callbacks **carry the live modifier state** (`modifiers` on
  `motion`/`button`/`axis`) and the gesture recognizer's output
  (`click_count` + the `dragging` latch on `button`) — the seat already
  has the full `AxlInputEvent`, so it forwards those fields rather than
  dropping them (Shift+wheel, ctrl/shift-click, and double/triple-click
  selection all read them; `click_count` in particular is timing the
  toolkit cannot reconstruct).  This is the idiom that bridges to **C++
  inheritance**: AGT fills one static-trampoline listener in its
  `AgtSurfaceHost` base, passes `this` as `user`, and dispatches to
  `virtual on_pointer_*` / `on_key` — written **once**, inherited by every
  widget-bearing window (DRY).  More Wayland-faithful *and* more
  inheritance-friendly than ad-hoc per-event callbacks.

### 2.4 The cursor (the top overlay)

The mouse cursor is just the **topmost overlay** over the composited
output — the one element that is always on top, is never hit-tested, and
moves at pointer-event rate.  `<axl/axl-cursor.h>` (`AxlCursor`) already
implements exactly this against a single back-buffer "scene": it
composites a sprite over the scene and erases by re-presenting the clean
scene region — no full-frame redraw (validated; see
[`AXL-Pointer-Cursor-Design.md`](AXL-Pointer-Cursor-Design.md) Option C).
It is the natural precursor here, not a separate thing to design later:

- **The mechanism carries over unchanged.**  AxlCursor's contract — *bind
  to the back-buffer being scanned out* — maps directly onto *bind to the
  compositor's composited output*.  When this compositor exists the cursor
  becomes its top overlay using the same sprite + source-over +
  `present_rect` path; nothing about the compositing changes.
- **Only ownership of input moves.**  AxlCursor tracks position and draws
  but deliberately never routes input.  Under the compositor the **seat**
  owns the pointer position (and grabs / hit-test); the cursor becomes a
  pure visual the compositor drives from the seat's pointer.
- **Cursor *shape* is per-surface, via the seat (inverted from Wayland,
  same effect).**  In Wayland the client supplies the cursor image with
  `wl_pointer.set_cursor(serial, surface, hot_x, hot_y)` on each pointer
  `enter` — so an I-beam appears over a text field, a resize arrow over a
  window edge.  Here the compositor owns the one cursor, so the focused
  surface instead *asks* for an image: a seat call
  (`axl_seat_set_cursor_image(image, hot_x, hot_y)`) wired straight to
  `axl_cursor_set_image`.  Without this seam every surface is stuck with
  the built-in arrow.  The seat resets to the default arrow on leave.
- **One implementation, two callers.**  Recommended: keep `AxlCursor`
  standalone (for non-compositor consumers — a boot menu, a tool that
  doesn't link the compositor) and have the compositor **use it
  internally** for its cursor overlay rather than growing a second
  cursor.  `axl_cursor_*` does not clash with `axl_surface_*` /
  `axl_seat_*`.

---

## 3. C API sketch (illustrative, not final)

```c
/* Surfaces — a tree (§2.1). parent == NULL → top-level (child of root).
   Position is relative to the parent; the subtree moves/raises/dies as
   one. create returns NULL on allocation failure (consumer handles it). */
AxlSurface *axl_surface_new(AxlSurface *parent, uint32_t w, uint32_t h);
void  axl_surface_free(AxlSurface *);               /* destroys the subtree */
void  axl_surface_set_parent(AxlSurface *, AxlSurface *parent);
void  axl_surface_move(AxlSurface *, int32_t x, int32_t y);  /* relative to parent */
void  axl_surface_resize(AxlSurface *, uint32_t w, uint32_t h);  /* preserves overlap; damages old∪new */
void  axl_surface_raise(AxlSurface *);   /* top of siblings; subtree travels */
void  axl_surface_lower(AxlSurface *);   /* bottom of siblings */
void  axl_surface_set_visible(AxlSurface *, bool);
void  axl_surface_set_opacity(AxlSurface *, uint8_t);  /* veil/dim */
void  axl_surface_set_opaque(AxlSurface *, bool);      /* occlusion hint (§7) */
void  axl_surface_set_input_region(AxlSurface *, const AxlGfxClip *, size_t);  /* NULL = full rect */
AxlGfxBuffer *axl_surface_buffer(AxlSurface *);         /* draw target */
void  axl_surface_damage(AxlSurface *, AxlGfxClip);
void  axl_surface_get_absolute(const AxlSurface *, int32_t *x, int32_t *y);  /* output coords of the origin */
void  axl_surface_to_output(const AxlSurface *, int32_t lx, int32_t ly, int32_t *ox, int32_t *oy);
void  axl_surface_from_output(const AxlSurface *, int32_t ox, int32_t oy, int32_t *lx, int32_t *ly);
void  axl_compositor_present(void);   /* the commit/atomicity barrier (§2.2) */

/* Per-surface input listener (Wayland wl_pointer_listener shape) — the
   C→C++-virtual bridge AGT writes once in its surface-host base (§2.3). */
typedef struct {
    void (*enter)(void *user, int32_t x, int32_t y);
    void (*leave)(void *user);
    void (*motion)(void *user, int32_t x, int32_t y, uint32_t modifiers);
    void (*button)(void *user, uint32_t button, bool pressed, int32_t x, int32_t y,
                   uint32_t modifiers, uint32_t click_count, bool dragging);
    void (*axis)(void *user, int32_t dx, int32_t dy, uint32_t modifiers);
    void (*key)(void *user, const AxlInputEvent *ev);
    void (*focus_in)(void *user);
    void (*focus_out)(void *user);
} AxlSurfaceListener;
void  axl_surface_set_listener(AxlSurface *, const AxlSurfaceListener *, void *user);

/* Seat — input focus + grabs */
void  axl_seat_pointer_grab(AxlSurface *);   /* push grab (dismiss on click outside its subtree) */
void  axl_seat_pointer_ungrab(void);         /* pop grab  */
void  axl_seat_set_keyboard_focus(AxlSurface *);
void  axl_seat_set_cursor_image(const AxlGfxBuffer *, int32_t hot_x, int32_t hot_y);  /* per-surface shape; NULL = arrow */
```

A class-based shape (`AxlCompositor`) is equally fine; the seam matters
more than the spelling.

---

## 4. The seam — what stays in the toolkit (AGT)

The compositor knows **surfaces and seats**, never **widgets**.  This is
the same line X11/Wayland draw, and it is load-bearing:

| Lives in the compositor (axl-gfx) | Stays in the toolkit (AGT) |
|---|---|
| Surfaces, z-order, compositing, present/damage | Widget tree, layout, drawing of controls |
| Raw input → **surface** routing | Hit-test **within** a surface → widget |
| Pointer **grab** (the menu/drag mechanism) | *Deciding* to grab (the menu bar's session logic) |
| **Applying** the cursor image (seat → overlay) | *Deciding* which cursor (I-beam over an edit widget, resize arrow over a border) |
| Per-**surface** keyboard focus | Per-**widget** focus: tab chain, focus policy, focus groups, radio-arrow-nav, focus restoration |

So the compositor delivers "keys go to surface S / pointer is grabbed by
surface S"; AGT decides which *widget* in S is focused and how Tab walks
them.  A display server has never done widget focus traversal, and
neither should this one.

---

## 5. How AGT maps onto it (migration, if/when built)

AGT already has the single-surface versions of all of this; the move is
mechanical and incremental:

| AGT today | → compositor |
|---|---|
| `AgtWindow::back_buf_` (one buffer) | a top-level `AxlSurface` (child of root) |
| popup **reparented** to paint last | a **child** `AxlSurface` parented to its window — real tree z-order; moves/raises/dies with the parent (§2.1) |
| `AgtApp` modal stack | surface subtree z + input confinement to the top subtree |
| `AgtWindow::captured_` (+ planned grab stack) | `axl_seat_pointer_grab` stack (dismiss on click outside the subtree) |
| `AgtWindow::focused_` | split: **surface** focus → seat; **widget** focus stays in AGT |
| per-window event dispatch | one `AxlSurfaceListener` trampoline in an `AgtSurfaceHost` base → C++ virtuals (§2.3) |
| dialog-veil blit/dim | a translucent surface composited over those beneath |

AGT's [`AGT-Input-Focus-Design.md`](../../agt/docs/AGT-Input-Focus-Design.md)
is **deliberately factored for this**: its grab stack + the "seat" notion
are the migration candidates that move down here; its focus
traversal/policy/groups stay up in AGT.  Until this compositor exists,
AGT implements the surface/seat parts itself in `AgtWindow`/`AgtApp`,
structured so they lift out cleanly later.

---

## 6. Sequencing / recommendation

Build the toolkit-side focus core in AGT **now** (it delivers the
features and the focus-traversal logic belongs in the toolkit regardless).
Treat this compositor as a **later extraction**, justified when one of
these is true:

1. A **second, non-AGT consumer** wants stacked surfaces + input (e.g. a
   SoftBMC on-screen dashboard, the network display server driving local
   surfaces, a minimal dialog in a tool that doesn't link AGT).
2. The **popup-reparent z-order hack** or back-buffer-readback class of
   bug becomes painful enough that real surfaces pay for themselves.

Until then this is a tracked target, not WIP.

**Building blocks already in place.**  The substrate now has what the
extraction needs — per-buffer damage + `axl_gfx_buffer_present_damage`
(G18), `axl_gfx_buffer_present_rect`, the source-over `axl_gfx_blend`,
`AxlCursor` (§2.4, the top overlay), and **`AxlNTree`** for the surface
tree itself: forward pre-order iteration (paint order), a reverse iterator
(`axl_ntree_iter_init_reverse` — topmost-first hit-test),
`axl_ntree_move_after`/`_before` (raise/lower/reparent), `is_ancestor`
(subtree grab/dismiss), and `free_full` (destroy cascade).  So the eventual
compositor is mostly **surface stacking + input routing over existing
primitives**, not new rendering or tree machinery — which lowers the cost
of building it when a trigger arrives.

## 7. Decisions (resolved) + what stays open

**Resolved** (folded into §2–§3 above):

- **Surface parenting → a tree (scene graph).**  Not a flat z-index:
  surfaces have an optional parent, children are relative-positioned, and
  the subtree moves / raises / hides / dies as a unit; stacking is tree
  pre-order.  This is Wayland's parent model (subsurface + popup parent)
  unified and collapsed for one in-process client.  See §2.1 — the
  load-bearing decision here.
- **Occlusion culling → a per-surface `opaque` bool** (not a region) for
  v0.1.  Wayland's `set_opaque_region`, 80/20'd: skip compositing under a
  surface that opaquely covers the damage rect.  Region-level occlusion is
  now scheduled as enhancement phase **E3** (§9), on the `AxlGfxRegion`
  primitive (E1).
- **Surface buffer budget → no hard cap; report OOM.**  Surfaces are
  content-sized; `axl_surface_new` returns NULL on alloc failure and
  the consumer handles it (same contract as `axl_malloc` /
  `axl_gfx_buffer_new`).  The compositor owns one full-screen output buffer
  buffer; everything else is content-sized.  Budget *policy* is the app's,
  not the compositor's (SOC).  Document the `Σ wᵢ·hᵢ·4 + W·H·4` cost.
- **Opacity / compositing math → reuse the gamma-correct `composite()` /
  `axl_gfx_blend` path**, per-surface opacity as a constant alpha folded
  in during the tree walk; the `opaque` flag is the skip-the-blend fast
  path.  Nothing new to build.
- **Event delivery → a per-surface `AxlSurfaceListener` (struct of fn
  ptrs + `user`)**, the Wayland-listener shape that bridges to C++
  virtuals once in AGT's surface-host base.  See §2.3 / §3.

**Still open:**

- **Does the network display server (`AXL-Display-Design.md`) target this
  same surface API** so remote + local share one model?  Direction: yes —
  the network server becomes a **client** of this local model (it owns the
  remote wire + a remote→surface adapter that creates/updates local
  `AxlSurface`s), keeping one compositing/stacking/present path (DRY) with
  transport vs compositing cleanly split (SOC).  Appealing, **not a v0.1
  commitment** — recorded so neither doc paints itself into a corner.
- **Keyboard model depth** — do surfaces want a Wayland-style xkb keymap
  /`modifiers`/`repeat_info`, or is `axl-input`'s `AxlInputEvent` (keycode
  + unicode + modifiers, repeat already handled by firmware typematic per
  `AXL-Pointer-Cursor-Design.md`) enough delivered surface-routed?  Lean
  toward the latter — the firmware already owns keymap + repeat; the seat
  just routes `AxlInputEvent` to the focused surface.

---

## 8. Implementation phases (when greenlit)

This is the build order.  Each phase is **test-first** (project
discipline): unit tests for the pure logic, plus a GOP integration
self-test under `run-qemu.sh --gpu` with `axl_gfx_capture` read-back (the
`cursor-selftest.c` / `test-gfx-present-qemu.sh` pattern), both arches,
ratcheted.  Each phase stands alone and leaves the tree green.

> **Status:** **C1–C6 LANDED** (C7 = AGT migration, consumer-side). C6 (the
> cursor — top overlay): the compositor owns one `AxlCursor` bound to its
> output; the seat drives it (`axl_compositor_pointer_event` moves + shows
> it, never writing the output — Option C), `axl_compositor_present` brackets
> its GOP flush with `axl_cursor_lift`/`drop` so it stays on top with no
> trail, and per-surface shape via `axl_compositor_set_cursor_image`
> (`axl_compositor_cursor` getter) with an auto-reset to the arrow on every
> pointer-focus change (a surface sets its shape from its `enter` callback).
> Unit-tested for position tracking + the never-writes-output invariant + the
> API; GOP `compositor-selftest` (33/33 both arches) verifies the sprite over
> the output, no trail on move, and the per-surface shape on enter / reset on
> leave. C5 (the seat — grabs +
> keyboard focus): pointer-grab LIFO stack (`axl_compositor_pointer_grab`/
> `_ungrab`, cap `AXL_COMPOSITOR_GRAB_MAX`) that confines pointer focus to
> the grab surface's subtree; a button press outside the subtree (incl.
> empty output) pops the grab and fires its `AxlGrabDismissFunc` (popped
> before the callback, so it may re-grab; nested dismiss re-enters the outer
> grab's surface). Per-surface keyboard focus
> (`axl_compositor_set_keyboard_focus`/`_keyboard_focus`, `focus_out`+
> `focus_in` via the listener) and `axl_compositor_key_event` routing
> KEY_DOWN/UP to the focused surface's `key` listener (raw `AxlInputEvent` —
> firmware owns keymap/repeat). `axl_compositor_attach_keyboard`/`_detach`
> over `axl_input_attach_key` (detach removes the loop source; a clean
> re-attach needs an `axl_input_detach_key` follow-on in axl-input).
> `axl_surface_free` purges the dying subtree from pointer focus,
> keyboard focus, and the grab stack without firing callbacks. No implicit
> drag-grab (use an explicit grab on press); no serial-auth (one trusted
> client). Unit-tested with synthetic events (grab confine/dismiss/inside-
> click/empty-space/nested/LIFO, keyboard focus + routing, destroy-purge,
> NULL-safety). C4 (the seat — pointer
> routing): hit-test topmost-first via the reverse pre-order iterator
> (honoring an optional per-surface input region, skipping the root and
> hidden subtrees); surface-local event delivery through an
> `AxlSurfaceListener` (the C→C++-virtual seam — `enter`/`leave`/`motion`/
> `button`/`axis` wired now, `key`/`focus_in`/`focus_out` declared for C5;
> the pointer callbacks carry `modifiers`, and `button` carries
> `click_count` + the gesture `dragging` latch, forwarded from the event);
> `enter`/`leave` on focus transitions and a synthetic re-hit-test after any
> surface change (create/destroy/move/raise/lower/set_parent/set_visible/
> set_input_region), gated on a seen-pointer flag so the GOP-less aa64 unit
> harness stays silent; `axl_surface_set_input_region` (Wayland semantics:
> NULL = full rect, non-NULL n==0 = input-transparent, else the n rects);
> `axl_compositor_pointer_event` (the testable routing core, output coords)
> + `axl_compositor_attach_pointer`/`_detach_pointer` (the device path over
> `axl_input_attach_mouse`, position unified with `AxlCursor` in C6). No
> grabs / keyboard / serial-auth yet (C5). Unit-tested with synthetic events
> (hit-test, surface-local coords, enter/leave, button change-bits, axis,
> re-hit-test on raise/hide/move/destroy, input regions, hidden-parent,
> NULL-safety, pointer gating). C3: per-surface
> `axl_surface_set_opacity` (255 = opaque copy fast path; <255 =
> source-over blend) via the new **gamma-aware** public
> `axl_gfx_composite` (honors `axl_gfx_set_gamma_correct`); a translucent
> surface can't occlude. Veil unit test (incl. a gamma-on assertion) + GOP
> read-back. Earlier: `<axl/axl-compositor.h>` +
> `src/gfx/axl-compositor.c`. C1: surfaces as embedded `AxlNTree` nodes,
> opaque pre-order compositing into a RAM output buffer, damage-bbox
> present. C2: `raise`/`lower`/`set_parent` (over the tree `move_*`
> primitives) + `set_opaque` with a top-level full-cover occlusion cull
> (`axl_compositor_composited_count` verifies it). Unit
> (`axl-test-compositor.c`: stacking, nested subtree, off-edge,
> raise/lower, reparent+cycle, occlusion incl. hidden floor) + GOP
> `compositor-selftest.c`, both arches. **Mid-point review done** (the
> tree core, per §8). C3+ pending.

| Phase | Delivers | Reuses | Test focus |
|---|---|---|---|
| **C1 — Skeleton: surfaces + composite + present** | `AxlSurface` (buffer/origin/size/visible/damage, create/destroy/move/buffer/damage) + `AxlCompositor` compositing visible surfaces in order into one output buffer, presenting damaged regions to GOP. The walking skeleton — pixels on screen. | `present_rect`, `present_damage` (G18), `axl_gfx_blend` | unit: surface state, damage bbox. GOP: N overlapping surfaces read back in correct stack order; only damaged region presented |
| **C2 — The surface tree** | `parent` in create, `set_parent`, `raise`/`lower` (sibling reorder), relative positioning (subtree moves), `show`/`hide`/`destroy` cascade; stacking = pre-order DFS.  `opaque` flag + occlusion skip. **Each `AxlSurface` wraps an `AxlNTree` node (data → back-ptr); the tree IS the surface tree** — so this phase is mostly geometry + thin wrappers, not new tree code. | **`AxlNTree`** — `move_after`/`move_before` (raise/lower/reparent), forward pre-order iter/traverse (paint order), `is_ancestor` (subtree), `free_full` (cascade) | unit: DFS paint order, subtree raise/move math, destroy cascade, occlusion skip. GOP: raise/move parent carries child; opaque surface culls below |
| **C3 — Compositing fidelity** | per-surface opacity (translucent veil over the stack, gamma-correct); geometry-change damage = `union(old,new)`. | `composite()` gamma path | GOP: veil opacity reads back correct; no trail after a move (uncovered region repainted) |
| **C4 — Seat: pointer routing** | hit-test **topmost-first** honoring the input region (walk the surface tree with `axl_ntree_iter_init_reverse`, stop at the first surface whose region contains the point); `enter`/`leave`/`motion`/`button`/`axis` delivered surface-local via `AxlSurfaceListener`; re-hit-test after any surface change (synthetic enter/leave). | **`AxlNTree` reverse iterator**; `axl-input` mouse source | unit: hit-test + input-region math; synthetic-event re-hit-test (à la the cursor-trampoline test) |
| **C5 — Seat: grabs + keyboard focus** | pointer-grab LIFO stack + dismiss-on-click-outside-subtree; per-surface keyboard focus + key routing + `focus_in`/`out`. | `axl-input` key source | unit: grab stack push/pop, outside-subtree dismiss detection, focus routing via synthetic events |
| **C6 — Cursor integration** | compositor drives `AxlCursor` as the top overlay from the seat pointer; `axl_seat_set_cursor_image` per-surface shape, reset to arrow on leave. | `<axl/axl-cursor.h>` | GOP: cursor composited over the output, no trail; shape changes on enter |
| **C7 — AGT migration** *(consumer-side, in AGT)* | `AgtSurfaceHost` base with the listener→virtual trampoline; map `AgtWindow`/popups onto the surface tree; retire the reparent hack + veil readback. | the C1–C6 API | AGT's own suite (out of scope for axl-sdk) |

C1–C3 are rendering, C4–C5 input/seat, C6 the cursor, C7 the consumer.
A natural **first stable-green review point** is end of C2 (the tree is the
load-bearing core; review it before the seat layers on top), per the
project's mid-point-review-for-large-changes rule.

## 9. Enhancement phases (post-C6)

C1–C6 are a correct, complete compositor for one client. These phases
raise **fidelity, performance, and confidence** — none is required for the
AGT migration (C7), but each pays off for a real consumer. The driving
observations:

- **Damage is a single bbox today** (`axl_gfx_buffer_add_damage` /
  `axl_compositor_present` coalesce all changes into one rectangle). AGT is
  a *text editor*: its per-frame changes are tiny and spatially disjoint —
  a blinking caret, a scrollbar thumb, a status-bar clock. The bbox union
  of two far-apart updates re-composites and flushes the whole spanning
  rectangle every frame. On the GOP/Blt path that is the difference between
  a smooth caret and a visibly churning screen.
- **Occlusion is a per-surface `bool`** (§7) — it can only cull a full-cover
  top-level. Wayland's `set_opaque_region` was deferred here.
- **The load-bearing tree + seat were verified largely by inspection +
  hand-written cases.** This session's input bug (a feature that passed
  inspection + unit mocks but was broken against real firmware) is the
  cautionary tale: mine a battle-tested reference and fuzz the invariants.

What unifies the first three is a missing primitive — **rectangle-set
algebra** (the role `pixman` plays in real Wayland). Build it once; damage,
occlusion, and the existing input-region rect arrays all consume it (DRY).

**E1 — `AxlGfxRegion` (exact rectangle-set algebra) — the enabling
substrate.** `<axl/axl-gfx-region.h>` + `src/gfx/axl-gfx-region.c`, in the
gfx module over the existing `AxlGfxClip` (a region IS a graphics concept —
every consumer is gfx/compositor; no `AxlRect` / cross-layer move). The
**pixman / X11 `miregion` model**: an EXACT set of non-overlapping rects in
canonical y-sorted bands, dynamically stored over `axl_array` (the backing
persists across present cycles — `clear()` resets the count, so there is no
steady-state allocation). `union` (add) / `subtract` / `intersect` all go
through ONE band **sweep** (cut the y-axis at every band edge of both
operands, combine the two x-span sets per strip, coalesce identical
adjacent bands), differing only in a per-band x-span combiner; the O(n)
`miregion` merge-walk is a tracked perf optimization over this — identical
results, only relevant if rect counts ever grow; plus `contains` (point) / `intersects` (rect) /
`bounds` / `is_empty` / `translate` / iteration. **No routine cap, no lossy
degrade** — exact at any complexity (the spike held avg 2.5 / worst ~46
rects over 24k fuzzed ops). The bounding-box superset is the fallback for
**`axl_array` OOM only** (the RELEASE OOM-injection path); a region flagged
`lossy` from OOM is treated conservatively per consumer (occlusion skips it
→ composite all; damage over-paints — both safe). Pure geometry, **no GOP,
fully unit-testable on both arches**. **Build + review first** — three
subsystems sit on it. Test strategy: exact-value unit asserts on the algebra
**plus a per-pixel bitmap-oracle fuzz** (rasterize the region, compare to a
brute-force grid under random union/subtract/intersect sequences — this is
the E4 property-test approach applied to the region, validated in the
spike). Spikes (`/tmp/spike-region*.c`, host-gcc, throwaway) settled the
design: bounded-lossy was prototyped and **rejected** in favor of exact
banded.

**E2 — Damage as a region (bbox → region). DONE (compositor) — `2c6fa717`.**
The compositor's `damage` is now an `AxlGfxRegion`: present flushes each
disjoint changed rect, and move/resize/reparent mark old + new subtree
bounds as separate rects. `axl_compositor_get_damage` is unchanged (returns
the region bounds = the same bbox); new `axl_compositor_get_damage_region`
exposes the precise set. Unit + GOP both arches.

**Per-buffer damage stays a bbox (deliberate, not done).** Making
`axl_gfx_buffer`'s damage a region would put an `AxlGfxRegion` on *every*
`AxlGfxBuffer` (a widely-created type) for marginal benefit to the few
direct-to-buffer apps — no motivating consumer (the AGT win is the
compositor, above). Those apps can compose `AxlGfxRegion` (E1) +
`axl_gfx_buffer_present_rect` directly for multi-rect present without the
per-buffer cost. Revisit only if a real direct-present consumer needs it.

**E3a — Damage-clipped composite. DONE.** `axl_compositor_present` now
recomposites ONLY the damaged region (each damage rect: clear + paint
surfaces clipped to it) instead of repainting the whole scene — a caret
blink re-blits its rect, not every surface. `axl_compositor_composite`
stays a full repaint for the standalone/initial path. Correct because every
visual-state change marks damage (audited: create/destroy/move/resize/
reparent/raise/lower/set_visible/set_opacity/damage; `set_opaque` is a perf
hint that doesn't change opacity=255 rendering, `set_input_region` is
hit-test only); app content changes follow the `axl_surface_damage`
contract. Note E2 already made the *expensive* GOP flush incremental; this
saves the RAM recomposite. Test: damage one of two disjoint surfaces →
`composited_count == 1` (only it re-blitted) + the other's pixels retained
(pixel read-back); discrimination confirmed (full repaint → count 2). Unit
+ GOP both arches.

**E3b — Region occlusion culling.** Generalize the top-level full-cover
floor cull to PARTIAL occlusion: a two-pass composite walk accumulates an
`occluded` region front-to-back (each opaque surface's rect), and each
surface below paints only its *visible* region = `rect ∩ clip − occluded`
(blitted per rect). So the hidden half of an opaque window overlapped by an
opaque window in front is never blitted. **At E3b the blit was binary**
(`opacity==255` straight copy, per-pixel alpha ignored / `<255` source-over),
so a surface was either fully opaque (255 → the whole rect occludes, captured
by the `opaque` bool) or uniformly translucent (<255 → can't occlude) — a
per-pixel opaque sub-region had no meaning, and `set_opaque_region` was
deferred. **E8 (below) reopened this** for the popup case: a per-pixel-alpha
surface blends its buffer's own alpha and never occludes. A per-pixel opaque
*sub-region* (`set_opaque_region`, claiming part of such a surface as opaque
for culling) is still NOT added — E8 surfaces simply never occlude. Test: an
opaque surface over a corner of a full-screen
opaque surface → the lower one paints as an L-shape (2 rects, not 1), pixel
read-back correct; discrimination (no occlusion → 1 rect) + an OOM-injection
test (any alloc failure in the walk → full-paint fallback → correct frame,
never an over-cull hole). Reuses E1 + E3a.

**E6 — Occlusion hoist (once-per-present). DONE.** E3b recomputed occlusion
(collect order + per-surface region ops) per damage rect. E6 hoists it: a
`CompVis` (draw order + each surface's *globally*-visible region = its screen
rect − everything opaque in front, over the full output) is built **once per
present** by `comp_vis_build`, then each damage rect is repainted by
intersecting it against the precomputed visible regions (`paint_clip_with_vis`).
Occlusion is a global property — independent of which sub-rect is repainted —
so this is exact: `(rect − occluded_global) ∩ D = (rect ∩ D) − (occluded ∩ D)`,
the same pixels the per-rect walk produced. This is wlroots' persistent
`node->visible` (§10). The OOM contract is unchanged (any region-op AXL_ERR or
alloc failure → `paint_clip_fallback`, a full clipped paint; the build still
counts as the one attempted pass). New introspection accessor
`axl_compositor_occlusion_passes` (mirrors `composited_count`) is the
discriminating signal: a present flushing N disjoint damage rects reports **1**,
not N. Test: a 2-rect present asserts `occlusion_passes == 1` (was 2 pre-hoist)
with culled pixels + blit count unchanged; the OOM test pins `== 1` on the
fallback path. Behavior-preserving — the existing occlusion/incremental/fuzz
suite is the safety net. Both arches + GOP.

**E4 — Property / fuzz invariants for the tree + seat. DONE.**
`test_compositor_fuzz` drives **2400 randomized op sequences** (60×40,
seeded xorshift) over create / move / raise / lower / set_parent /
set_visible / set_opaque / destroy / keyboard-focus / pointer-grab /
pointer-event, with a parent-tracked pool so subtree destroys stay
consistent. After every op it asserts the strong observable invariant —
**keyboard focus is always NULL or a LIVE surface** (a dangling focus after
destroy is a use-after-free) — plus crash-freedom (each op is followed by a
`present`, exercising the occlusion walk + the destroy-purge of focus/grab
over the random tree) and present-clears-damage. Discrimination verified:
neutering the destroy→keyboard-focus purge makes the fuzz fail. Pure logic,
both arches. This is the "tests catch what inspection misses" hardening
applied to the load-bearing core.

**E5 — `wlr_scene` cross-read (reference validation, not code).** Read
wlroots' scene-graph (MIT-licensed C — damage accumulation, direct-scanout
/ occlusion, node stacking) and the relevant `wayland-protocols`
(`xdg-shell`, `wp_viewporter`), and record in §10 of this doc any
divergence from our model worth **adopting or explicitly rejecting**. The
compositor analog of the EDK2 read that caught the input bug. Output is a
doc delta, not a commit — and it ideally precedes/informs E2–E3.

**E7 — Frame callbacks (present throttling). DONE.** Wayland `wl_surface.frame`
adapted to the single-client synchronous model: `axl_surface_request_frame(s,
cb, user)` registers a one-shot per-surface callback (latest-wins; `cb == NULL`
cancels; destroying the surface cancels it via tree unlink — no stale `user`
fire). `axl_compositor_dispatch_frame(c, time_ms)` is the testable core — it
**snapshots** the pending `(cb, user)` set, clears each surface's flag, then
fires, so a re-request made *inside* a callback queues for the NEXT dispatch
(the throttle); it returns whether any callback is still pending. Callbacks are
**time-based** (each gets `axl_time_get_ms`), so cadence jitter never corrupts
animation. `axl_compositor_attach_frame_clock(c, loop, interval_ms)` wires it to
a self-cancelling `axl_loop_add_timer`: each tick dispatches, and the timer
removes itself (`AXL_SOURCE_REMOVE`) once nothing re-requests — idle = no busy
wakeups — re-arming on the next request. Precondition (documented): a callback
may mutate/damage/present/re-request but must NOT create/destroy surfaces
(defer teardown past present), which keeps the snapshot-then-fire safe; an OOM
snapshot skips the frame (callbacks stay pending), non-fatal. Tests: pure-core
(synthetic time, no loop) pins one-shot firing, the dispatch timestamp,
re-request-queues-next, NULL-cancel, destroy-cancel, NULL-safety; a loop-driven
test drives a real timer through animate → self-cancel (`REMOVE` branch) →
re-arm. Both arches. **Per-surface, not compositor-level** — Wayland-faithful
and gives free destroy-cleanup (a dead surface is unlinked → never walked);
the root may hold a screen-wide transition callback. **Adoption recipe:**
`sdk/examples/frame-anim-demo.c` (`make frame-anim-demo`) shows the pattern a
toolkit uses — one `request_frame` per window surface driving a per-window
animation *coordinator* that ticks every active animation by elapsed time,
redraws + presents once, and re-requests only while something animates (two
animations, one throttled present; the clock self-cancels when motion stops).
This replaces the per-widget `axl_loop_add_timer` pattern (one timer + present
per animated widget).

**E8 — Per-pixel-alpha blit at full opacity. DONE** (`d7091409`). Reopens the
E3b deferral. The blit was binary: `opacity==255` straight copy (the buffer's
per-pixel alpha *ignored* — the fast opaque path) / `<255` whole-surface
source-over (a uniform fade). A popup / dropdown / tooltip is an opaque panel
PLUS a soft drop shadow in ONE surface — opaque body, partial-alpha edges,
transparent gaps — which neither path renders correctly.
`axl_surface_set_per_pixel_alpha(s, on)` flags a 255-opacity surface onto the
source-over path so its buffer's OWN alpha blends untouched (`a = buffer.alpha
· 255/255`): opaque body replaces, soft edges blend, transparent gaps show the
backdrop through, with no whole-surface dimming. Such a surface never occludes
(its gaps are see-through), so `surf_opaque_rect` excludes it regardless of the
`opaque` hint. Test (`test_compositor_per_pixel_alpha`): straight-copy default
(transparent pixel copied raw), then body-replaces / gap-shows-backdrop /
edge-is-source-over / no-cull (`composited_count == 2`), all exact-value.
Unblocks AGT C7 Phase 4 (popups as child surfaces) + Phase 6 (dialog veil).
Landed by the consumer (AGT C7); design reconciled here.

**E9 — Chain pointer grabs (popup chains). DONE.** The grab was exclusive:
confine strictly to the top grab's subtree and suspend everything below (the
modal model — correct for a dialog over a menu). But a menu chain (bar → menu →
submenu) is sibling popup surfaces that must ALL stay interactive, which a
top-only grab can't span (hovering submenu → parent falls "outside"), forcing
the toolkit's manual cross-surface routing. `axl_compositor_pointer_grab_chain`
pushes a grab whose confinement EXTENDS the grab below it: the active region is
the union of a contiguous run of chain grabs from the top of the stack down
through the first EXCLUSIVE grab. So hover and clicks route across the whole
open chain; a press outside ALL of it pops the top grab and dismisses (the
toolkit closes the chain). `axl_compositor_pointer_grab` stays exclusive (a
modal opened over a chain still blocks it); a lone chain grab confines to its
own subtree, exactly like an exclusive one. `seat_grab_filter` generalized to
test the active set. Test: a 2-popup sibling chain, both chain-grabbed → the
LOWER grab still routes (discriminates vs top-only) + a press outside the chain
dismisses; the modal / nested / LIFO exclusive-grab tests stay green.

**Slice 3 topology — two valid menu architectures (consumer's choice).** AGT C7
Slice 3 locked the **nested** design; E9 enables a **flat** alternative. Both
lift the menu bar to a session surface (the bar titles must be inside the grab
for hover-switch + click-away dismiss, and a window widget can't be); they
differ only in how the menus below it are parented:
- **Nested (AGT's locked design):** menu = child of the bar surface, submenu =
  child of its parent menu. ONE exclusive `axl_compositor_pointer_grab` on the
  bar confines the whole nested subtree. Cost: surface-local coordinates
  **cascade per level** (a submenu's position is relative to its parent menu
  surface, recursively) — flagged as the design's gotcha.
- **Flat + chain (E9):** bar, menu, and submenu are all window-child *sibling*
  surfaces (as slices 1–2 already parent popups); push one
  `axl_compositor_pointer_grab_chain` per open popup, so the chain's union is
  the confinement. Cost: N chain grabs (push/pop per popup) instead of one.
  Win: **uniform window-relative coordinates** everywhere (no per-level
  cascade) — every popup positions in output coords like the combo/tooltip.
The trade is *nested coords + 1 grab* vs *flat uniform coords + N chain grabs*.
For a deep submenu cascade the flat coords are materially simpler; for a single
menu level the nested design is a touch less bookkeeping. E9 exists so the
consumer can pick; the locked design uses the exclusive grab and needs nothing
new from the compositor.

**E10 — Backdrop blur (the dialog veil). DONE.** The veil was a back-buffer
READBACK in the toolkit (snapshot the parent's composited pixels, blur,
alpha-fill) — the bug class C7 Phase 6 set out to kill (the dialog-veil-black
regression lived here). `axl_surface_set_backdrop_blur(s, radius)` moves it into
the compositor: when a flagged surface is composited, the output region it
covers — which, because compositing is back-to-front, already holds everything
beneath it — is blurred in place (the G6 `axl_gfx_buffer_blur` stack blur)
BEFORE the surface's tint is blitted on top. Combine with `opacity < 255` /
per-pixel-alpha (the veil tint) so the frosted backdrop shows through; the
surface is excluded from occlusion (a translucent overlay). Scratch-OOM skips
the blur (un-frosted, not fatal). **A present never re-blurs a bare damage
sub-rect** — that would clamp the blur mid-surface and SEAM (a caret blink in
the dialog card above the veil). Two ways out, and present takes the cheaper
one whenever it can prove them equivalent:

- *Partial re-blur (the fast path).* A blur is a bounded neighbourhood read,
  so a change under the veil only moves the frost within `radius` of it, and
  computing THAT exactly needs raw backdrop within `2*radius`. Present
  recomposites the damage grown by `2*radius` and clipped to the veil (the
  blur *halo*), `blur_valid_rect` writes back only the part of it the blur got
  exact (the halo eroded by `radius`, except on edges that are the veil's own
  — there the clamp is the intended one), and the collateral — recomposited
  but neither damaged nor re-frosted — is saved before the paint and restored
  after. The flush set is the damage plus the re-frosted rects, so an
  untouched veil is not pushed to the GOP. Measured (`BENCH-COMP-CARET`, x64
  RELEASE under KVM): a 12x18 caret under a full-screen 1280x800 veil went
  from 31.6 ms to 0.09 ms per present. A blur that cannot allocate mid-paint
  restores the whole snapshot and flushes nothing — the frame is dropped rather
  than left with an un-frosted patch no later halo-sized present would repaint.
- *Whole-veil re-blur (the fallback).* `backdrop_blur_expand` /
  `veil_expand_full` union the surface's full rect into the damage. Taken when
  the plan cannot be proven equivalent: veils that overlap each other (the
  upper one's blur SOURCE is the lower one's frosted output, exact only where
  the lower one re-frosted), a veil whose blit is split by an opaque surface
  in front of it (its blur then clamps at the occluder's edge, which the
  erosion cannot model), more than `COMP_MAX_VEILS` veils, or an allocation
  failure anywhere in the plan — including an OOM-degraded (lossy) region,
  which would flush stale pixels or restore over fresh ones.

Either way a present that contains a veil composites ONE rect, so the region
banding can't split a veil into separately-blurred pieces, and the blur always
clamps at the surface's own edge (a near-fullscreen veil's = the screen edge →
invisible). Test: an EXACT oracle — blur the same pattern independently with
`axl_gfx_buffer_blur` and require a pixel-for-pixel match (off → sharp edge;
on → matches the oracle + the sharp edge becomes a midtone), a PRESENT-path
partial-damage repaint that still matches the full-blur oracle (no seam;
discrimination-verified), and no-occlusion. On top of that
`compositor-selftest.c` compares the WHOLE frame — read back from the
framebuffer, so the flush set is pinned too — against the full-blur oracle
across edge/corner/1x1/large/disjoint/repeated damages, a non-fullscreen
dialog veil, and stacked overlapping veils; a single probe pixel cannot see a
seam. Unblocks AGT C7 Phase 6 (delete the veil readback + the
clip-aware-`clear()` constraint it forced).

**Situational (deferred — consumer-gated, not yet phased):**
- **Output scale (HiDPI)** — a global scale factor for high-DPI panels;
  cheap, but no consumer is asking yet.
- **`wp_viewporter`-style crop/scale** — surface content scaled/cropped to
  a different output size. Becomes load-bearing *only* if the §7 "network
  display server as a client of the surface model" direction is pursued
  (remote surface scaled to the local output). Let that question drive it.

| Phase | Delivers | Reuses | Test focus |
|---|---|---|---|
| **E1 — `AxlGfxRegion`** ✅`3d9a8600` | EXACT banded rect-set (`union`/`subtract`/`intersect`/`contains`/`intersects`/`bounds`/`translate`) over `axl_array`; bbox-superset on OOM only | `AxlGfxClip`, `axl-array` | unit (both arches): algebra + **bitmap-oracle fuzz** |
| **E2 — Compositor damage as a region** ✅`2c6fa717` | compositor `damage` is an `AxlGfxRegion`; present flushes each rect; `get_damage` bbox retained + `get_damage_region`. (Per-buffer damage stays a bbox — deliberate.) | E1, `present_rect` (G18) | unit + GOP: disjoint damages → separate flushes |
| **E3a — Damage-clipped composite** ✅`8ad9e581` | present recomposites only the damaged region, not the whole scene | E1, E2 | unit (`composited_count`) + GOP |
| **E3b — Region occlusion culling** ✅`a2ccfdcf` | two-pass front-to-back opaque-region cull; hidden parts of overlapped opaque surfaces not blitted. (`set_opaque_region` rejected — straight-copy blit.) | E1, E3a | unit (L-shape blit-count) + OOM-injection + GOP |
| **E4 — Property/fuzz invariants** ✅`4716e25f` | 2400 random tree+seat op sequences; focus-live + crash-free + present-clears invariants | the C1–C6 API | unit (both arches), discrimination-verified |
| **E5 — `wlr_scene` cross-read** ✅ | reference-validated occlusion/damage/stacking vs wlroots; divergences recorded | — (research) | §10 (this doc) |
| **E6 — Occlusion hoist (once-per-present)** ✅ | `CompVis` built once per present (draw order + global visible regions), each damage rect intersected against it; `occlusion_passes` accessor | E3b, E1 | unit (`occlusion_passes == 1` for an N-rect present + OOM-path) + the E3b/incremental/fuzz safety net |
| **E7 — Frame callbacks (present throttling)** ✅ | per-surface one-shot `request_frame`; testable `dispatch_frame` (snapshot+fire, re-request queues next) + `has_pending_frames`; self-cancelling loop frame clock (`attach/detach_frame_clock`) | C1 tree, `axl-loop` timer, `axl-time` | unit core (synthetic time) + loop-driven (real timer: animate → self-cancel → re-arm) |
| **E8 — Per-pixel-alpha blit** ✅`d7091409` | `axl_surface_set_per_pixel_alpha` — a 255-opacity surface blends its buffer's OWN alpha (opaque body + soft/transparent gaps, the popup case) and is excluded from occlusion. Reopens the E3b deferral. | E3b blit + occlusion | unit (body/gap/edge/no-cull, exact-value) |
| **E9 — Chain pointer grabs** ✅ | `axl_compositor_pointer_grab_chain` — confinement spans the union of a contiguous chain-grab run (popup chains: menu → submenu); exclusive `pointer_grab` unchanged (modals). | C5 grab stack | unit (lower-chain-grab routes; modal/nested/LIFO stay green) |
| **E10 — Backdrop blur** ✅ | `axl_surface_set_backdrop_blur` — blurs the composited backdrop under a surface (the dialog veil), excluded from occlusion; reuses the G6 stack blur | E8 blit + occlusion, `axl_gfx_buffer_blur` | unit (exact blur-oracle match + no-cull) |

**Sequencing.** E1 first (prove the primitive — **spike now**, settle the
API, then land test-first). E5 ideally read **before/during** E2–E3 so the
reference informs the damage + occlusion design. A **mid-point review**
after E1 + E2 (the substrate + the damage rework) before E3 layers
occlusion on it, per the large-change rule. E4 is orthogonal and can land
at any point — earlier is better (it guards everything underneath).

## 10. Reference cross-read — `wlr_scene` (E5)

Validation of the E1–E4 model against wlroots' scene-graph (`wlr_scene`,
read from a clone of `gitlab.freedesktop.org/wlroots/wlroots`,
`types/scene/wlr_scene.c` + `include/wlr/types/wlr_scene.h`). The point is
to confirm our occlusion / damage / stacking choices against a
battle-tested compositor and to record, per divergence, whether to **adopt**,
**reject**, or **defer** — the compositor analog of the EDK2 read that
caught the input bug.

**Validated (our model matches wlroots):**
- **Occlusion = front-to-back opaque subtraction.** `scene_node_update_iterator`
  maintains a running visible region and, per node, subtracts that node's
  opaque region so nodes below get less visible area
  (`pixman_region32_subtract(data->visible, data->visible, &opaque)`). This
  is exactly E3b's `vis = rect − occluded` walk. Our approach is the right
  one.
- **Opaque only when fully opaque.** `scene_node_opaque_region` contributes
  nothing unless `opacity == 1` (and, for a rect, `color[3] == 1`) — matching
  our `opacity == 255` gate in `surf_opaque_rect`. A translucent node can't
  occlude in either compositor.
- **Region algebra is the substrate.** wlroots leans on `pixman_region32`
  everywhere (visible, opaque, damage); we built `AxlGfxRegion` (E1) for the
  same role. Confirmed this is the load-bearing primitive.
- **Synchronous-vs-scheduled.** wlroots schedules frames + presentation
  feedback; we present synchronously for one in-process client (§7). Its
  frame machinery is exactly the per-client plumbing we collapsed away.

**Deliberate divergences (simpler, and correct for our constraints):**
- **No damage ring / buffer-age.** wlroots keeps a `wlr_damage_ring` per
  output to union the last N frames' damage (a back buffer in a
  double/triple-buffered swapchain must repaint everything changed since it
  was last current). We present a **single** output buffer to the GOP via
  `present_rect`, so there is no buffer age — one damage region per present
  is exactly right. **Reject** the damage ring (it solves a problem we don't
  have).
- **Opaque BOOL + a per-pixel-alpha flag, not a per-pixel opaque region.**
  wlroots culls with a per-buffer `opaque_region` (pixman) OR a
  `buffer_is_opaque` full-rect flag. We use the `opaque` bool for full-rect
  occluders; **E8 added `per_pixel_alpha`** for the popup case (opaque body +
  soft / transparent edges in one surface) — such a surface blends its buffer's
  own alpha and is excluded from occlusion entirely. wlroots' per-region
  `opaque_region` (claiming a SUB-rect of a per-pixel-alpha surface as opaque to
  still cull behind it) stays **deferred**: our per-pixel-alpha surfaces are
  small overlays (popups, the veil) where culling the body's footprint isn't
  worth the per-surface region cost. Revisit only if a large per-pixel-alpha
  surface with a big opaque interior ever sits over expensive content.

**Optimizations wlroots confirms (deferred, with a known-good design):**
- **Persistent per-node visible region.** wlroots stores `node->visible`
  and updates it *incrementally* (`subtract update_region; union visible`)
  rather than recomputing per frame. **ADOPTED in E6** (the
  once-per-present form): occlusion is now computed once per present into a
  `CompVis` (draw order + each surface's global visible region) and each
  damage rect is intersected against it, instead of recomputing the walk per
  damage rect. We rebuild `CompVis` each present rather than mutating a
  persistent per-node region incrementally — the surface set is small and a
  present already touches every node, so the full rebuild-once is simpler than
  tracking incremental `node->visible` deltas across tree edits, with the same
  asymptotic win (1 walk per present, not N). Going fully incremental (persist
  `node->visible`, update only on tree change) was **profiled and deferred** —
  see below.
  - **Profiled (`test/integration/compositor-bench.c`).** The metric is an
    *animation frame*: a present that changes only a small region while the
    geometry is unchanged still does a full O(N) occlusion REBUILD after E6, so
    `CompVis` work is exactly what a persistent `node->visible` would save. On a
    1920×1080 scene with overlap + partial occlusion (release build, QEMU —
    absolute µs are emulated; the **scaling** and **frame-budget fraction** are
    the load-bearing numbers), the per-frame rebuild cost (tiny-damage present,
    1 blit) was: **N=8 → 36 µs, N=32 → 193 µs, N=128 → 1.14 ms, N=256 →
    3.09 ms** — i.e. **0.2 % / 1.2 % / 6.8 % / 18.5 %** of a 16.67 ms (60 fps)
    frame. The cost is **superlinear** (≈O(N²): each surface subtracts against
    an `occluded` region that grows with N).
  - **Conclusion: deferred.** At realistic scales the rebuild is negligible —
    a UEFI/AGT scene has a handful to a few dozen *compositor surfaces* (toolkit
    widgets live inside a surface's buffer drawn by the toolkit, NOT as separate
    surfaces), where the rebuild is <1.5 % of a frame. The persistent-region
    caching/invalidation complexity is not justified there. It only crosses ~5 %
    of budget above **N≈128** top-level surfaces. **Revisit** the incremental
    `node->visible` (or a cheaper occluder structure that kills the O(N²)
    `occluded`-subtract growth) only if a consumer animates a scene that large.
- **Direct scanout.** When a single buffer covers the output,
  `scene_output_build_state` scans it out directly and skips compositing
  (`WLR_SCENE_DISABLE_DIRECT_SCANOUT` toggles it). Our analog: when a
  full-cover opaque surface's buffer matches the GOP format/stride, present
  it directly instead of compositing into the output buffer. **Defer** —
  needs format/stride matching against the GOP; compositing into our one
  output buffer + `present_rect` is correct and simple meanwhile.

**Net:** the cross-read found no flaw in E1–E4 and no must-adopt gap; it
confirmed the occlusion/region design, justified the three simplifications
our single-client single-buffer UEFI target allows, and validated the two
optimizations it identified — the persistent visible region (since **adopted
in E6** as the once-per-present hoist) and direct scanout (deferred; needs GOP
format/stride matching).
