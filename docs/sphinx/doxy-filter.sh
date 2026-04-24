#!/bin/bash
# Doxygen input filter for AXL headers.
#
# 1. Convert opening doc comment to plain comment (avoids duplicate
#    file description — the .rst files provide module intros).
# 2. Replace allocation #define macros with typed function prototypes
#    so Doxygen renders return types and parameter types.

sed \
  -e '1s|^/\*\*|/*|' \
  -e 's|^#define axl_malloc(size) .*|void *axl_malloc(size_t size);|' \
  -e 's|^#define axl_calloc(count, size) .*|void *axl_calloc(size_t count, size_t size);|' \
  -e 's|^#define axl_realloc(ptr, size) .*|void *axl_realloc(void *ptr, size_t size);|' \
  -e 's|^#define axl_free(ptr) .*|void axl_free(void *ptr);|' \
  -e 's|^#define axl_strdup(s) .*|char *axl_strdup(const char *s);|' \
  -e 's|^#define axl_memdup(src, size) .*|void *axl_memdup(const void *src, size_t size);|' \
  -e 's|^#define axl_new(Type) .*|void *axl_new(size_t Type);|' \
  -e 's|^#define axl_new_array(Type, Count) .*|void *axl_new_array(size_t Type, size_t Count);|' \
  "$1"
