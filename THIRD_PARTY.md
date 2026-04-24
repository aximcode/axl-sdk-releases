# Third-party components

This AXL SDK distribution statically links the following third-party
components into `libaxl.a` and into the pre-built tool `.efi` binaries
shipped in the `axl-sdk-tools-*.tar.gz` tarballs. Each component retains
its original copyright and license. The full license text of each
component is included in the `third_party/<component>/` subdirectory
next to this file.

Redistributors of binaries built against this SDK must preserve these
attributions.

## mbedtls

- **Source:** https://github.com/Mbed-TLS/mbedtls
- **Version shipped:** 3.6.3
- **Vendored path in source tree:** `deps/mbedtls/`
- **Copyright:** The Mbed TLS Contributors
- **License:** Dual-licensed under
  [Apache License 2.0](https://spdx.org/licenses/Apache-2.0.html)
  **OR**
  [GNU General Public License v2.0 or later](https://spdx.org/licenses/GPL-2.0-or-later.html).
  **This distribution elects the Apache 2.0 license.**
- **Full license text:** `third_party/mbedtls/LICENSE`

The mbedtls `3rdparty/everest/` and `3rdparty/p256-m/` subdirectories
contain code from separate upstream projects (Project Everest and
mpg/p256-m, respectively). Both are distributed within mbedtls under
compatible Apache 2.0 terms; see the mbedtls LICENSE file for details.

No modifications have been made to the vendored mbedtls source.
