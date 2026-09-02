# Handoff — `AXL_INSECURE_FETCH`: make the hash the trust anchor for every SDK fetch

> Self-contained. Everything below was measured against this tree (VERSION
> 4.5.0) and the flagship consumer on 2026-09-02; where a claim is inherited it
> says so. Excluded from the public snapshot (D5), so it may name internal
> paths.

## 0. START HERE — state, task, why

**axl-sdk:** `main`, VERSION **4.5.0** (just cut, new asset shape published to
`aximcode/axl-sdk-releases`). This is a **new, small change**, not part of the
D-series — but it lands on the **D7 critical path** (see §5): the flagship
consumer's migration should pin the release that carries this fix, so cut it
**before** telling axl-utils to migrate.

**The task, in one sentence:** the SDK already treats "the pinned SHA256 is the
trust anchor, not TLS" everywhere it matters — except its own downloads still
use plain `curl`, so behind a corporate MITM proxy without that proxy's CA they
fail at TLS even though the hash would catch any tampering. Add an opt-in
`AXL_INSECURE_FETCH=1` that makes those already-hash-verified fetches skip TLS
verification (`curl -k`), keeping the `sha256sum -c` as the guarantee.

**Why now:** a real coworker on a Dell laptop without the corporate CA in WSL
ran the consumer's `make install-sdk`. The SDK package installed (the consumer
fetches it with `curl -k`), then `axl-install-toolchain` failed:

```
[install-toolchain] downloading arm-gnu-toolchain-…-aarch64-none-elf.tar.xz …
curl: (60) SSL certificate problem: self-signed certificate in certificate chain
```

Requiring every such user to install the Dell CA chain is the status quo we are
removing — they never have, and it is per-machine setup the hash makes
unnecessary. Dell MITMs HTTPS org-wide, so this hits **any** consumer or direct
SDK user behind the proxy, not one person.

**Verified the mechanism is sound (measured, not assumed):** with a CA store
that does not trust the presented cert (`--cacert /dev/null`, a faithful
"no trusted CA" stand-in), plain `curl` to the real ARM URL returns `http=000`
and `curl --insecure` returns `http=302`; `insecure` bypasses the failure while
the download's own `sha256sum -c` still rejects tampering. The proxy re-signs
but relays file bytes unchanged, so the pinned hash still matches — and would
fail correctly if it did not. TLS was never the guarantee here; the hash is.

## 1. Exactly what to change

Both fetch sites **already** verify a pinned SHA256, so `-k` removes nothing
that was protecting them:

| file | lines | fetch | already hash-checks? |
|---|---|---|---|
| `scripts/install-toolchain.sh` | 217 (aa64), 262 (x64) | `curl -fL … "$URL"` | yes — `sha256sum -c` at 220 / 264 |
| `install.sh` (the release asset) | 150 (VERSION), 225 (SHA256SUMS), 235 (asset) | `curl -fsSL …` | yes — verifies every asset against SHA256SUMS |

**The change:** honor `AXL_INSECURE_FETCH=1` (default unset = today's behavior,
TLS **and** hash). When set, add `-k` to those `curl` invocations. Suggested
shape, one place per script:

```sh
# The download is verified against a pinned SHA256, which is strictly stronger
# than TLS for a fixed known artifact. AXL_INSECURE_FETCH=1 lets a consumer
# behind a corporate MITM proxy trust that hash instead of the proxy's CA.
CURL_TLS=$([ "${AXL_INSECURE_FETCH:-0}" = 1 ] && echo -k)
curl $CURL_TLS -fL … "$URL"
```

- **`install.sh` is the source of record**; the copy in `stage/` / `out/…` is
  generated. Change the source and let the build regenerate it — do not edit
  the staged copies (there are three; `check-snapshot-clean` / the build will
  drift if you hand-edit).
- Keep it **opt-in**. Default stays TLS+hash so non-corporate users get defense
  in depth and a `-k` never appears unrequested. The consumer declares the
  MITM environment explicitly — an auditable contract, not a blanket `curl -k`
  that reads as disabled security to a reviewer.
- `install.sh`'s own bootstrap fetch (the user's `curl -fsSLO …/install.sh`
  before it runs) is **out of scope** — it is the caller's curl, not the
  script's. The consumer already fetches `install.sh` with `curl -k`.

## 2. Also: name the way out in the error

Today the SDK's `install-toolchain.sh` just aborts on the failed `curl`
(`set -e`); the helpful "install the Dell CA chain" text a coworker saw is in
the **consumer's** wrapper (`install-axl-sdk.sh`), not here — a direct SDK user
gets no guidance. When the toolchain fetch fails, print a hint naming both ways
out: install the corporate CA, **or** `AXL_INSECURE_FETCH=1` if the environment
MITMs HTTPS and you trust the pinned hash.

## 3. Record the contract

`AXL_INSECURE_FETCH` is a public knob — document it where the trust model
lives. `docs/AXL-Distribution-Design.md` §14 already states "the hash is the
trust anchor"; add the env var there (and to `install.sh --help` and the
releases-repo README) as the sanctioned escape hatch for a proxy-hostile host.
This is a deliberate, auditable widening of an already-accepted tradeoff — note
it, do not bury it.

## 4. Verify + cut 4.5.1

```
# local gate first (CI contends on this box — check `gh run list`):
verify.sh                      # 26 gates green
# the mechanism, both settings:
AXL_INSECURE_FETCH=1 <exercise a fetch against a distrusted cert> → succeeds
unset;                <same>                                       → fails (unchanged)
# hash still bites when set:
AXL_INSECURE_FETCH=1 <corrupt the artifact> → sha256sum -c rejects
scripts/cut-release.sh 4.5.1   # prompts before pushing; publishes the new-shape assets + this fix
```

4.5.1 is a patch: no asset-shape change, no API change — a fetch-robustness
knob. `check-published-release.sh` (D4) runs on it as usual.

## 5. How this threads into D7

The consumer migration (axl-utils, its own handoff
`doc/axl-sdk-migration-handoff.md`) was about to pin **4.5.0**. With this fix it
should pin **4.5.1** instead and set `AXL_INSECURE_FETCH=1` when it invokes the
SDK's toolchain install — one line, mirroring the `curl -k` it already uses for
the SDK assets, and it needs **no** consumer-side TLS shim. So the order is:

1. Land this change, cut **4.5.1**.
2. Tell axl-utils it can migrate, pinning 4.5.1.
3. D7's last step (delete `scripts/build-packages.sh`) is unchanged and still
   gated on that migration landing.

Do **not** hand axl-utils a consumer-side `curl` shim as a substitute — it was
considered and rejected: it couples to the SDK's internal use of `curl` (the
SDK ships Python tools; a future `urllib` fetch would silently defeat it) and
every corporate consumer would have to reinvent it. The fix belongs here.
