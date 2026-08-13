#!/bin/bash
# Doxygen input filter for AXL headers.
#
# Replace allocation #define macros with typed function prototypes so
# Doxygen renders return types and parameter types.
#
# There used to be a `1s|^/**|/*|` rule here to avoid a duplicate file
# description. It predates every header carrying @file, and it silently
# stripped the @file from axl-crashrecord.h -- the only header whose line 1
# is /** -- leaving it unreferenceable. The .rst module intros come from
# src/*/README.md, which never collided with these; descriptions render once.

sed \
  -e 's|^#define axl_malloc(size) .*|void *axl_malloc(size_t size);|' \
  -e 's|^#define axl_calloc(count, size) .*|void *axl_calloc(size_t count, size_t size);|' \
  -e 's|^#define axl_realloc(ptr, size) .*|void *axl_realloc(void *ptr, size_t size);|' \
  -e 's|^#define axl_free(ptr) .*|void axl_free(void *ptr);|' \
  -e 's|^#define axl_strdup(s) .*|char *axl_strdup(const char *s);|' \
  -e 's|^#define axl_memdup(src, size) .*|void *axl_memdup(const void *src, size_t size);|' \
  -e 's|^#define axl_new(Type) .*|void *axl_new(size_t Type);|' \
  -e 's|^#define axl_new_array(Type, Count) .*|void *axl_new_array(size_t Type, size_t Count);|' \
  "$1"
