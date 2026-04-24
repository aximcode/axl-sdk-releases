Stream-based I/O with three layers:

1. **Simple helpers** (GLib-style): `axl_print`, `axl_file_get_contents`
2. **Stream I/O** (POSIX-style): `axl_fopen`, `axl_fread`, `axl_fprintf`
3. **Low-level**: `axl_read`, `axl_write`, `axl_pread`, `axl_pwrite`

All strings are UTF-8. Paths are converted to UCS-2 internally.

Header: `<axl/axl-io.h>`

## Overview

AXL provides familiar POSIX-style I/O on top of UEFI's file protocols.
Paths use UTF-8 with forward slashes (`fs0:/path/to/file.txt`); AXL
converts to UCS-2 and backslashes internally.

## Console Output

`axl_printf` writes to the UEFI console (ConOut). It's available
immediately after `AXL_APP` or `axl_driver_init`:

```c
axl_printf("Hello %s, value=%d\n", name, 42);
axl_printerr("Error: %s\n", msg);  // writes to ConErr
```

## File Read/Write

The simplest way to read or write files:

```c
// Read entire file into memory
void *data;
size_t len;
if (axl_file_get_contents("fs0:/config.json", &data, &len) == 0) {
    // process data...
    axl_free(data);
}

// Write entire file
axl_file_set_contents("fs0:/output.txt", buf, buf_len);
```

## Stream I/O

For line-by-line reading or incremental writes:

```c
AxlStream *f = axl_fopen("fs0:/log.txt", "r");
if (f != NULL) {
    char *line;
    while ((line = axl_readline(f)) != NULL) {
        axl_printf("  %s\n", line);
        axl_free(line);
    }
    axl_fclose(f);
}
```

## Buffer Streams

In-memory streams for building data without files:

```c
AxlStream *buf = axl_open_buffer();
axl_fprintf(buf, "name=%s\n", name);
axl_fprintf(buf, "value=%d\n", value);

void *data;
size_t len;
axl_stream_get_bufdata(buf, &data, &len);
// data contains the formatted text
axl_fclose(buf);
```

## File Operations

```c
bool exists = axl_file_exists("fs0:/data.bin");
bool is_dir = axl_file_is_dir("fs0:/logs");

axl_file_delete("fs0:/temp.txt");
axl_file_rename("fs0:/old.txt", "fs0:/new.txt");
axl_dir_mkdir("fs0:/output");
```
