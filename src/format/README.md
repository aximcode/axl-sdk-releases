# AxlFormat -- Printf Engine

Callback-driven printf engine. Format text directly into any sink
(buffer, network socket, file, hash) without intermediate allocation.

This is the engine behind `axl_printf`, `axl_snprintf`, `axl_asprintf`,
and `axl_string_append_printf`. It has **zero dependencies** (no memory
allocator, no I/O) -- it breaks the Log -> Data circular dependency by
being self-contained.

Header: `<axl/axl-format.h>`

## Callback-Driven Formatting

The core API takes a write callback that receives formatted output
in chunks. No memory is allocated -- all formatting uses a small
stack buffer.

```c
#include <axl.h>

// Write directly to a TCP socket
void net_write(const char *data, size_t len, void *ctx) {
    axl_tcp_send((AxlTcp *)ctx, data, len, 0);
}

// Format an HTTP request line directly into the socket
axl_format(net_write, sock, "GET /%s HTTP/1.1\r\nHost: %s\r\n\r\n",
           path, host);
```

## API

Two functions:

- `axl_format(write_fn, ctx, fmt, ...)` -- variadic convenience
- `axl_vformat(write_fn, ctx, fmt, args)` -- va_list version

The callback type:

```c
typedef void (*AxlWriteFunc)(const char *data, size_t len, void *ctx);
```

## Use Cases

- Format directly into a network send buffer (no intermediate string)
- Stream formatted output into a hash computation
- Write a custom logging backend that formats in-place
- Build protocol messages without allocating temporary strings

## Supported Format Specifiers

| Specifier | Type | Example |
|-----------|------|---------|
| `%s` | `char *` string | `axl_printf("%s", name)` |
| `%d` | signed int | `axl_printf("%d", -42)` |
| `%u` | unsigned int | `axl_printf("%u", 42)` |
| `%x` / `%X` | hex (lower/upper) | `axl_printf("0x%x", 0xFF)` |
| `%llu` | `uint64_t` | `axl_printf("%llu", big)` |
| `%zu` | `size_t` | `axl_printf("%zu", len)` |
| `%c` | char | `axl_printf("%c", ch)` |
| `%p` | pointer | `axl_printf("%p", ptr)` |
| `%%` | literal `%` | `axl_printf("100%%")` |

Width and zero-padding are supported: `%08x`, `%-20s`, `%5d`.
