#!/bin/sh
# post-install scriptlet run by dpkg/rpm after axl-sdk is unpacked.
# Prints a minimal "getting started" hint so the user knows where to look.

echo "axl-sdk installed. Try:"
echo "  axl-cc --version"
echo "  axl-cc /usr/share/doc/axl-sdk/examples/hello.c -o hello.efi"
