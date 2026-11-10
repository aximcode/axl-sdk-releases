# ed25519-ref10 (vendored)

Ed25519 signing and verification, as OpenSSH bundles it.

| | |
|---|---|
| Upstream | https://github.com/openssh/openssh-portable |
| Tag | `V_10_5_P1` |
| Commit | `b3f7344209832eea8ece447d871ea748767c444b` |
| Licence | Public domain |
| Derived from | `supercop-20221122` (per the file's own comments) |

`ed25519.c` is **byte-identical to upstream** (SHA-256
`ac65b7840b922f11d7618320714f5cf22bd6cece8efab146e9688c70ffbe9895`, 202411
bytes, 2064 lines). Its authors are Daniel J. Bernstein, Niels Duif, Tanja
Lange, Peter Schwabe (lead) and Bo-Yin Yang. The public-domain dedication is
in its own header comment; upstream ships no separate `LICENSE` file for it,
which is why **this README is what the release tarball stages** as the
component's licence record.

Two files sit **beside** it rather than replacing anything in it:

- `crypto_api.h` is **trimmed**: reduced to the typedefs, sizes and prototypes
  `ed25519.c` uses, with upstream's `#include "includes.h"` dropped (it pulls
  OpenSSH's autoconf world), `randombytes` left as a prototype rather than a
  macro over `arc4random_buf`, and `crypto_hash_sha512` left as a prototype
  rather than a `static inline` over OpenSSL's `EVP_Digest`. Everything else
  upstream declares — SNTRUP761, ML-KEM-768, ML-DSA-44/65/87, the SHA-3
  utilities, `small_random32` — is dropped; `ed25519.c` references none of it.
- `includes.h` is **not upstream**. It is empty. `ed25519.c`'s first line is
  `#include "includes.h"`, so an empty shim on the include path is what lets
  the vendored source stay byte-identical instead of having that line deleted.

`OPENSSL_HAS_ED25519` must stay undefined — `ed25519.c` guards its entire body
with `#ifndef OPENSSL_HAS_ED25519`, so defining it would compile the file to
nothing. Nothing in this tree defines it, and the unit test that calls
`crypto_sign_ed25519_keypair_from_seed` would fail to link if anything did.

## Why OpenSSH's copy

OpenSSH is the conformance oracle for this code's first consumer (AxlSsh), so
interop bugs surface against the implementation we are actually talking to.

## What AXL supplies

Beyond the C library (`memcpy`, `memmove`, `memset`) and the stack
protector (`__stack_chk_fail`, `__stack_chk_guard`), which this tree
supplies to every object, `ed25519.c` calls three things provided
specifically for it by `src/net/axl-ed25519-glue.c`:

- `crypto_hash_sha512` — over mbedTLS's one-shot `mbedtls_sha512()`, the
  same code `AxlChecksum`'s `AXL_CHECKSUM_SHA512` wraps, but with its context
  on the stack
- `explicit_bzero` — a non-elidable scrub
- `randombytes` — over `axl_crypto_rng()`

The vendored code ignores the hash's result at all five call sites, and
`randombytes` is `void` upstream, so a failure in either reaches it as a
digest or a seed of **all zeros** — and no value is safe to hand it instead.
Measured (2026-09-20) with a hash that could fail:

- a failed **nonce** hash makes r = 0: R is the identity, S = h·a, and the
  signature **verifies** while disclosing the private scalar, a = S·h⁻¹ mod L;
- a failed **challenge** hash makes h = 0, and R = encode(B), S = 1 then
  verifies under **every** public key;
- a zeroed seed is a valid key whose private half is public.

So the glue **counts** every failure, and `src/net/axl-pk-ed25519.c` reads the
count before and after each call into this code and refuses the operation if
it moved, whatever the call returned. The digest and seed are still zeroed,
but that is hygiene; the refusal is what makes a failure safe.
`test/unit/axl-test-ed25519.c` holds all three consequences through a test
hook in the glue, and its allocation sweeps hold that the hash allocates
nothing.

The hash cannot fail in the shipped configuration: `mbedtls_sha512()` fails
only for a bad argument, or when a compressor mbedTLS does not own reports
one. The glue `#error`s on the two options that install such a compressor —
`MBEDTLS_SHA512_ALT`, and `MBEDTLS_SHA512_PROCESS_ALT` unless
`AXL_SHA512_PROCESS_ALT_CANNOT_FAIL` says the replacement returns success
unconditionally (the Ed25519 design spec's E4 plans exactly that replacement).
AXL's own key generation never reaches `randombytes`: it draws the seed from
`axl_crypto_rng()` itself, where a failure is checkable, and the unit test
pins that it makes no `randombytes` call.

## Which SUPERCOP variant this is, and what it costs

**This is SUPERCOP's `ref`, not `ref10`** — the file's own provenance comments
say `supercop-20221122/crypto_sign/ed25519/ref/…` on every one of its ten
section headers. The directory name is inherited from the design spec and the
E2b plan, which both assumed `ref10`; the code is what it is.

The difference is not cosmetic. `ref` represents a field element as
`crypto_uint32 v[32]` (radix 2^8, 128 bytes) where `ref10` uses `int32 [10]`
(radix 2^25.5, 40 bytes), so the precomputed base-point table is
`ge25519_aff[425]` = **108,800 bytes**, not the 30,720 a `ref10`
`ge_precomp[32][8]` would be. Measured on the x64 `-Os` object, 2026-09-05:

| | bytes |
|---|---|
| `.rodata` | 109,956 (of which `ge25519_base_multiples_affine` is 108,800) |
| `.text` | 11,827 |
| **object total** | **121,783** |

That makes the weak-symbol seam in `src/net/axl-pk-provider.h` **more**
load-bearing than its own docstring claims, not less: an image that does not
opt in avoids ~119 KB, not ~30 KB.

## Exported symbols

`ed25519.c` `#define`s its internals to a `crypto_sign_ed25519_ref_` prefix
(see its `fe25519.h` and `ge25519.h` sections), so **every** symbol it exports
already carries that prefix — including the three the E2b plan expected to
find bare (`fe25519_getparity`, `ge25519_unpackneg_vartime`,
`ge25519_isneutral_vartime`). Verified with `nm` against a built `libaxl.a`:
eight globals, none of them defined anywhere else in the archive, and no
duplicate global symbol in the archive at all. Nothing needs renaming, which
is fortunate, because renaming would mean editing the vendored file.

## Compiling it

Built by a per-rule recipe in the `Makefile` that adds `-Ideps/ed25519-ref10`
for the two files above. It needs **no** warning suppression: it compiles clean
under this tree's `-Wall` and under `scripts/lint.sh`'s clang `-Wall -Wextra`
(measured 2026-09-05), unlike `deps/lzma` and `deps/libvterm`. `deps/` is
outside clang-tidy's scope, which only walks `src/`.

## Resyncing

Re-copy `ed25519.c` byte-identical from the new tag, re-run the trimmed header
against the compiler, and update the commit and hash above. There are no local
patches to reapply, and keeping it that way is deliberate: see the design
spec's §6b for what a patch would cost.
