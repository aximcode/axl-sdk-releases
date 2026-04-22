# AXL SDK — Releases

Download page for [AXL SDK](https://axl.aximcode.com), a GLib-inspired
C library and SDK for building UEFI applications and drivers without
EDK2. Development happens in a separate private repository; binary and
source packages for each tagged release are published here.

## Install

**Debian / Ubuntu**

```bash
curl -LO https://github.com/aximcode/axl-sdk-releases/releases/latest/download/axl-sdk_amd64.deb
sudo apt install ./axl-sdk_amd64.deb
```

**Fedora / RHEL**

```bash
curl -LO https://github.com/aximcode/axl-sdk-releases/releases/latest/download/axl-sdk.x86_64.rpm
sudo dnf install ./axl-sdk.x86_64.rpm
```

**TLS flavor** (for HTTPS via mbedTLS): replace `axl-sdk` with
`axl-sdk-tls` in the filenames above.

Each binary package bundles both x64 and aa64 UEFI target libraries —
one install covers cross-compilation for both architectures. See the
[release notes](https://github.com/aximcode/axl-sdk-releases/releases)
for per-version details and the full list of artifacts (binaries,
source tarballs, SHA256SUMS).

## Documentation

- [API reference & guides](https://axl.aximcode.com/)
