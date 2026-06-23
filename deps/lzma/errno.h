/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */
/* errno.h -- AXL-authored minimal errno shim for freestanding UEFI builds.
   Not part of the LZMA SDK; provides only the errno constants the SDK
   headers reference so they compile without a hosted C library. */
#ifndef LZMA_FREESTANDING_ERRNO_H
#define LZMA_FREESTANDING_ERRNO_H

#define EINVAL  22
#define EEXIST  17
#define ENOENT   2
#define ENOSPC  28
#define ENOMEM  12
#define EBADF    9

#endif /* LZMA_FREESTANDING_ERRNO_H */
