/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AximCode */

/**
 * axl-mem.h:
 *
 * Memory allocation with dmalloc-inspired debug features.
 *
 * Size-tracking header enables realloc without old_size.
 * DEBUG builds add fence-post guards, alloc/free fill patterns,
 * file/line tracking, and leak reporting.
 *
 * Do NOT free axl_malloc'd memory with FreePool (it has a header).
 */

#ifndef AXL_MEM_H
#define AXL_MEM_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#include <axl/axl-macros.h>

// ---------------------------------------------------------------------------
// Allocation (macros hide the _impl functions — see axl-mem-impl.h)
// ---------------------------------------------------------------------------

/**
 * @brief Allocate @a size bytes of uninitialized memory.
 * @param size number of bytes to allocate
 * @return pointer to allocated memory, or NULL on failure.
 *     Free with axl_free().
 */
#define axl_malloc(size)        axl_malloc_impl((size), __FILE__, __LINE__)

/**
 * @brief Allocate zero-initialized memory for @a count elements
 *     of @a size bytes each.
 * @param count number of elements
 * @param size  size of each element in bytes
 * @return pointer to allocated memory, or NULL on failure.
 *     Free with axl_free().
 */
#define axl_calloc(count, size) axl_calloc_impl((count), (size), __FILE__, __LINE__)

/**
 * @brief Resize a previously allocated block to @a size bytes.
 *
 * Contents are preserved up to the smaller of old and new sizes.
 * If @a ptr is NULL, behaves like axl_malloc().
 *
 * @param ptr  pointer from axl_malloc/axl_calloc/axl_realloc, or NULL
 * @param size new size in bytes
 * @return pointer to reallocated memory, or NULL on failure
 *     (original block is unchanged).
 */
#define axl_realloc(ptr, size)  axl_realloc_impl((ptr), (size), __FILE__, __LINE__)

/**
 * @brief Free memory allocated by axl_malloc, axl_calloc, axl_realloc,
 *     axl_strdup, or axl_memdup. NULL-safe.
 * @param ptr pointer to free, or NULL
 */
#define axl_free(ptr)           axl_free_impl(ptr)

/**
 * @brief Duplicate a NUL-terminated string.
 * @param s string to duplicate, or NULL
 * @return newly allocated copy, or NULL on failure.
 *     Free with axl_free().
 */
#define axl_strdup(s)           axl_strdup_impl((s), __FILE__, __LINE__)

/**
 * @brief Duplicate @a size bytes of memory from @a src.
 * @param src  source buffer
 * @param size number of bytes to copy
 * @return newly allocated copy, or NULL on failure.
 *     Free with axl_free().
 */
#define axl_memdup(src, size)   axl_memdup_impl((src), (size), __FILE__, __LINE__)

/**
 * @brief Allocate and zero-initialize a single instance of a type.
 *
 * Usage: <tt>MyStruct *p = axl_new(MyStruct);</tt>
 *
 * @param Type the type to allocate (passed as a type name)
 * @return typed pointer, or NULL on failure. Free with axl_free().
 */
#define axl_new(Type)              ((Type *)axl_calloc(1, sizeof (Type)))

/**
 * @brief Allocate and zero-initialize an array of elements.
 *
 * Usage: <tt>int *arr = axl_new_array(int, 100);</tt>
 *
 * @param Type  the type to allocate (passed as a type name)
 * @param Count number of elements
 * @return typed pointer, or NULL on failure. Free with axl_free().
 */
#define axl_new_array(Type, Count) ((Type *)axl_calloc((Count), sizeof (Type)))

// ---------------------------------------------------------------------------
// Container macros
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Auto-cleanup (RAII) — __attribute__((cleanup))
// ---------------------------------------------------------------------------

/**
 * @brief Auto-free a heap pointer when it goes out of scope.
 *
 * Works with axl_malloc, axl_strdup, axl_memdup, and any pointer
 * freed by axl_free().
 *
 * @code
 * AXL_AUTO_FREE char *s = axl_strdup("hello");
 * if (error) return -1;  // s is freed automatically
 * @endcode
 *
 * IMPORTANT: Always initialize at declaration. Never use with goto
 * that jumps over the declaration. Cleanup runs at scope exit
 * regardless of whether the variable was assigned.
 */
void axl_free_impl(void *ptr);  /* forward decl for cleanup */
static inline void
_axl_auto_free_func(void *p)
{
    axl_free_impl(*(void **)p);
}
#define AXL_AUTO_FREE  __attribute__((cleanup(_axl_auto_free_func)))

// ---------------------------------------------------------------------------
// Utility macros
// ---------------------------------------------------------------------------

/**
 * @brief Get the number of elements in a static array.
 *
 * Only works on actual arrays, not pointers. Produces a compile-time
 * constant suitable for use in static initializers.
 */
#define AXL_ARRAY_SIZE(a)  (sizeof(a) / sizeof((a)[0]))

/**
 * @brief Construct a 32-bit signature from four ASCII characters.
 *
 * Encodes characters in little-endian order. Commonly used for
 * structure validation signatures in UEFI drivers.
 */
#define AXL_SIGNATURE_32(a, b, c, d)  \
    ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

/**
 * @brief Derive a pointer to the enclosing structure from a member pointer.
 *
 * Given a pointer to a struct member, returns a pointer to the
 * containing structure. Equivalent to Linux kernel's container_of
 * and EDK2's CR/BASE_CR macros.
 *
 * @param ptr    pointer to the member
 * @param type   type of the containing structure
 * @param member name of the member within the structure
 */
#define AXL_CONTAINER_OF(ptr, type, member)  \
    ((type *)((char *)(ptr) - __builtin_offsetof(type, member)))

// ---------------------------------------------------------------------------
// Page-aligned allocation (contiguous physical memory)
// ---------------------------------------------------------------------------

/**
 * @brief Allocate contiguous page-aligned memory.
 *
 * Allocates @a count pages (each 4096 bytes) of contiguous physical
 * memory. Suitable for DMA buffers, RAM disks, and other uses
 * requiring physical address alignment.
 *
 * @return 0 on success, -1 on error.
 */
int
axl_alloc_pages(
    size_t    count,     ///< number of 4KB pages to allocate
    uint64_t *phys_addr  ///< [out] receives physical address
);

/**
 * @brief Free page-aligned memory allocated by axl_alloc_pages.
 */
void
axl_free_pages(
    uint64_t  phys_addr,  ///< physical address from axl_alloc_pages
    size_t    count        ///< number of pages (must match alloc)
);

// ---------------------------------------------------------------------------
// Statistics and debugging
// ---------------------------------------------------------------------------

typedef struct {
    size_t  count;
    size_t  bytes;
    size_t  total_count;
    size_t  total_bytes;
} AxlMemStats;

/**
 * @brief Get current allocation statistics.
 */
void axl_mem_get_stats(
    AxlMemStats *stats  ///< [out] receives statistics
);

/**
 * @brief Print all outstanding allocations to the log (debug builds).
 *
 * Each leaked block is reported with its size, file, and line number.
 * No-op in release builds.
 */
void axl_mem_dump_leaks(void);

/**
 * @brief Validate a heap pointer's fence-post guards (debug builds).
 * @return true if valid, false if corrupted or not an axl_malloc'd pointer.
 *     Always returns true in release builds.
 */
bool axl_mem_check(
    const void *ptr  ///< pointer to validate
);

/**
 * @brief Inject an out-of-memory failure for testing error paths.
 *
 * After calling `axl_mem_fail_next_alloc(N)` with N > 0, the Nth
 * subsequent allocation through the AXL allocator
 * (`axl_malloc`/`axl_calloc`/`axl_realloc`/`axl_strdup`/`axl_memdup`)
 * returns NULL without touching the backend. Allocations before the
 * Nth succeed normally. After the failure fires, the counter resets
 * to 0 (disabled) so subsequent allocations also succeed.
 *
 * @code
 * // Exercise the OOM path of some constructor.
 * axl_mem_fail_next_alloc(1);
 * MyThing *t = my_thing_new();
 * assert(t == NULL);
 * @endcode
 *
 * Pass 0 to disable injection. DEBUG builds only — in release
 * builds this is a no-op and allocations always proceed.
 */
void axl_mem_fail_next_alloc(
    size_t n  ///< fail the Nth next alloc (1 = next, 0 = disabled)
);

// Implementation details for the axl_malloc/free macros
#include <axl/axl-mem-impl.h>

#ifdef __cplusplus
}
#endif

#endif /* AXL_MEM_H */
