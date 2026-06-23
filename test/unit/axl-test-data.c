/** @file axl-test-data.c
    Test application for AxlData — hash, array, string, JSON.
**/

#include "axl-test.h"

#include <stdint.h>

// ---------------------------------------------------------------------------
// Hash Table Tests
// ---------------------------------------------------------------------------

static size_t foreach_count;

static void
foreach_counter(const void *key, void *value, void *data)
{
    (void)key;
    (void)value;
    (void)data;
    foreach_count++;
}

static void
test_hash(void)
{
    AxlHashTable *t;
    size_t val1 = 100;
    size_t val2 = 200;
    size_t val3 = 300;
    void *got;
    bool removed;

    // --- axl_hash_table_new (string keys, copied internally) ---

    t = axl_hash_table_new_str();
    test_check(t != NULL, "hash: new");
    if (t == NULL) {
        return;
    }

    axl_hash_table_insert(t, "alpha", &val1);
    axl_hash_table_insert(t, "beta", &val2);
    axl_hash_table_insert(t, "gamma", &val3);
    test_check(axl_hash_table_size(t) == 3, "hash: size is 3");

    got = axl_hash_table_lookup(t, "beta");
    test_check(got == &val2, "hash: get beta");

    got = axl_hash_table_lookup(t, "missing");
    test_check(got == NULL, "hash: get missing returns NULL");

    // Overwrite
    axl_hash_table_insert(t, "alpha", &val3);
    got = axl_hash_table_lookup(t, "alpha");
    test_check(got == &val3, "hash: overwrite alpha");
    test_check(axl_hash_table_size(t) == 3, "hash: size still 3 after overwrite");

    // Remove
    removed = axl_hash_table_remove(t, "beta");
    test_check(removed, "hash: remove beta");
    test_check(axl_hash_table_size(t) == 2, "hash: size is 2 after remove");
    got = axl_hash_table_lookup(t, "beta");
    test_check(got == NULL, "hash: get removed returns NULL");

    // Foreach
    foreach_count = 0;
    axl_hash_table_foreach(t, foreach_counter, NULL);
    test_check(foreach_count == 2, "hash: foreach count is 2");

    axl_hash_table_free(t);

    // --- Resize test with axl_hash_table_new (copied string keys) ---

    t = axl_hash_table_new_str();
    for (size_t i = 0; i < 128; i++) {
        char key_buf[16];
        axl_snprintf(key_buf, sizeof(key_buf), "key%03u", (unsigned)i);
        axl_hash_table_insert(t, key_buf, (void *)(size_t)(i + 1));
    }
    test_check(axl_hash_table_size(t) == 128, "hash: 128 entries after bulk insert");

    got = axl_hash_table_lookup(t, "key000");
    test_check(got == (void *)(size_t)1, "hash: get key000 after resize");

    got = axl_hash_table_lookup(t, "key127");
    test_check(got == (void *)(size_t)128, "hash: get key127 after resize");

    axl_hash_table_free(t);
}

static size_t free_count;

static void
test_free_counter(void *data)
{
    (void)data;
    free_count++;
}

static void
test_hash_new_full(void)
{
    // new_full with key_free + value_free
    AxlHashTable *t = axl_hash_table_new_full(
        NULL, NULL, test_free_counter, test_free_counter);
    test_check(t != NULL, "hash_full: new");
    if (t == NULL) {
        return;
    }

    // Insert 3 entries (keys/values are just pointers, but free_counter tracks calls)
    free_count = 0;
    axl_hash_table_insert(t, "a", (void *)1);
    axl_hash_table_insert(t, "b", (void *)2);
    axl_hash_table_insert(t, "c", (void *)3);
    test_check(axl_hash_table_size(t) == 3, "hash_full: size is 3");

    // Replace with same key pointer — old value freed, key not freed
    // (same pointer, so key_free is skipped as a self-free guard)
    free_count = 0;
    axl_hash_table_insert(t, "a", (void *)10);
    test_check(free_count == 1, "hash_full: replace calls value_free");
    test_check(axl_hash_table_size(t) == 3, "hash_full: size still 3 after replace");

    // Remove — should call both destructors
    free_count = 0;
    axl_hash_table_remove(t, "b");
    test_check(free_count == 2, "hash_full: remove calls key_free + value_free");
    test_check(axl_hash_table_size(t) == 2, "hash_full: size is 2 after remove");

    // Free remaining — should call destructors for 2 entries (4 calls)
    free_count = 0;
    axl_hash_table_free(t);
    test_check(free_count == 4, "hash_full: free calls destructors for all entries");
}

static void
test_hash_contains(void)
{
    AxlHashTable *t = axl_hash_table_new_str();

    axl_hash_table_insert(t, "present", NULL);
    axl_hash_table_insert(t, "valued", (void *)42);

    test_check(axl_hash_table_contains(t, "present"), "contains: NULL-valued key exists");
    test_check(axl_hash_table_contains(t, "valued"), "contains: valued key exists");
    test_check(!axl_hash_table_contains(t, "absent"), "contains: absent key false");

    // get returns NULL for both absent and NULL-valued — contains distinguishes
    void *got = axl_hash_table_lookup(t, "present");
    test_check(got == NULL, "contains: get NULL-valued returns NULL");

    axl_hash_table_free(t);
}

static void
test_hash_steal(void)
{
    AxlHashTable *t = axl_hash_table_new_full(
        NULL, NULL, test_free_counter, test_free_counter);

    axl_hash_table_insert(t, "keep", (void *)1);
    axl_hash_table_insert(t, "steal_me", (void *)2);
    test_check(axl_hash_table_size(t) == 2, "steal: size is 2");

    // Steal should NOT call destructors
    free_count = 0;
    bool stolen = axl_hash_table_steal(t, "steal_me");
    test_check(stolen, "steal: returns true");
    test_check(free_count == 0, "steal: no destructors called");
    test_check(axl_hash_table_size(t) == 1, "steal: size is 1");

    // Steal missing key
    stolen = axl_hash_table_steal(t, "nonexistent");
    test_check(!stolen, "steal: missing returns false");

    axl_hash_table_free(t);
}

static bool
remove_even_values(const void *key, void *value, void *data)
{
    (void)key;
    (void)data;
    size_t v = (size_t)value;
    return (v % 2) == 0;
}

static void
test_hash_foreach_remove(void)
{
    AxlHashTable *t = axl_hash_table_new_str();

    for (size_t i = 0; i < 10; i++) {
        char key[8];
        axl_snprintf(key, sizeof(key), "k%zu", i);
        axl_hash_table_insert(t, key, (void *)i);
    }
    test_check(axl_hash_table_size(t) == 10, "foreach_rm: size is 10");

    // Remove entries with even values (0, 2, 4, 6, 8)
    size_t removed = axl_hash_table_foreach_remove(t, remove_even_values, NULL);
    test_check(removed == 5, "foreach_rm: removed 5 even entries");
    test_check(axl_hash_table_size(t) == 5, "foreach_rm: size is 5");

    // Verify odd entries remain
    test_check(axl_hash_table_lookup(t, "k1") == (void *)1, "foreach_rm: k1 remains");
    test_check(axl_hash_table_lookup(t, "k3") == (void *)3, "foreach_rm: k3 remains");
    test_check(axl_hash_table_lookup(t, "k0") == NULL, "foreach_rm: k0 removed");
    test_check(axl_hash_table_lookup(t, "k2") == NULL, "foreach_rm: k2 removed");

    axl_hash_table_free(t);
}

static void
test_hash_iter(void)
{
    AxlHashTable *t = axl_hash_table_new_str();

    axl_hash_table_insert(t, "x", (void *)1);
    axl_hash_table_insert(t, "y", (void *)2);
    axl_hash_table_insert(t, "z", (void *)3);

    // Basic iteration — count entries
    AxlHashTableIter iter;
    axl_hash_table_iter_init(&iter, t);
    size_t count = 0;
    void *key;
    void *value;
    while (axl_hash_table_iter_next(&iter, &key, &value)) {
        count++;
    }
    test_check(count == 3, "iter: iterated 3 entries");

    // Iteration with remove
    axl_hash_table_iter_init(&iter, t);
    while (axl_hash_table_iter_next(&iter, &key, &value)) {
        if ((size_t)value == 2) {
            axl_hash_table_iter_remove(&iter);
        }
    }
    test_check(axl_hash_table_size(t) == 2, "iter: size 2 after iter_remove");
    test_check(axl_hash_table_lookup(t, "y") == NULL, "iter: y removed");
    test_check(axl_hash_table_lookup(t, "x") != NULL, "iter: x remains");
    test_check(axl_hash_table_lookup(t, "z") != NULL, "iter: z remains");

    // Iteration with steal
    axl_hash_table_insert(t, "w", (void *)4);
    axl_hash_table_iter_init(&iter, t);
    while (axl_hash_table_iter_next(&iter, &key, &value)) {
        if ((size_t)value == 4) {
            axl_hash_table_iter_steal(&iter);
        }
    }
    test_check(axl_hash_table_size(t) == 2, "iter: size 2 after iter_steal");
    test_check(axl_hash_table_lookup(t, "w") == NULL, "iter: w stolen");

    axl_hash_table_free(t);
}

static void
test_hash_direct(void)
{
    // Test direct hash/equal (pointer keys)
    AxlHashTable *t = axl_hash_table_new_full(
        axl_direct_hash, axl_direct_equal, NULL, NULL);
    test_check(t != NULL, "direct: new");
    if (t == NULL) {
        return;
    }

    axl_hash_table_insert(t, (void *)42, (void *)100);
    axl_hash_table_insert(t, (void *)99, (void *)200);

    void *got = axl_hash_table_lookup(t, (void *)42);
    test_check(got == (void *)100, "direct: get 42");

    got = axl_hash_table_lookup(t, (void *)99);
    test_check(got == (void *)200, "direct: get 99");

    got = axl_hash_table_lookup(t, (void *)0);
    test_check(got == NULL, "direct: get missing");

    axl_hash_table_free(t);
}

static void
test_hash_insert_vs_replace(void)
{
    // insert: on collision, destroy NEW key, keep OLD key
    // replace: on collision, destroy OLD key, keep NEW key
    // Both destroy the old value.

    free_count = 0;

    AxlHashTable *t = axl_hash_table_new_full(
        NULL, NULL, test_free_counter, test_free_counter);

    // Use distinct heap-allocated keys that compare equal
    char *key_a1 = axl_strdup("key_a");
    char *key_a2 = axl_strdup("key_a");

    // First insert — no collision; return value should be 1 (new entry)
    free_count = 0;
    AxlHashTableInsertResult rc = axl_hash_table_insert(t, key_a1, (void *)1);
    test_check(rc == AXL_HASH_TABLE_NEW, "ins_vs_rep: first insert returns NEW");
    test_check(free_count == 0, "ins_vs_rep: first insert no frees");
    test_check(axl_hash_table_size(t) == 1, "ins_vs_rep: size 1");

    // insert with collision — NEW key (key_a2) should be freed, OLD key (key_a1) kept;
    // return value should be 0 (replaced)
    free_count = 0;
    rc = axl_hash_table_insert(t, key_a2, (void *)2);
    test_check(rc == AXL_HASH_TABLE_REPLACED, "ins_vs_rep: collision insert returns REPLACED");
    // Expect: key_destroy(key_a2) + value_destroy(old value)
    test_check(free_count == 2, "ins_vs_rep: insert collision frees new key + old value");
    test_check(axl_hash_table_size(t) == 1, "ins_vs_rep: size still 1");

    // The stored value should be the new one
    void *got = axl_hash_table_lookup(t, "key_a");
    test_check(got == (void *)2, "ins_vs_rep: insert updated value");

    axl_hash_table_free(t);
    /* test_free_counter doesn't actually free — reclaim test fixture keys
     * explicitly. Both keys went through the destroy callback (key_a2 at
     * the insert collision; key_a1 at axl_hash_table_free). */
    axl_free(key_a1);
    axl_free(key_a2);

    // Now test replace behavior
    t = axl_hash_table_new_full(
        NULL, NULL, test_free_counter, test_free_counter);

    char *key_b1 = axl_strdup("key_b");
    char *key_b2 = axl_strdup("key_b");

    rc = axl_hash_table_replace(t, key_b1, (void *)10);
    test_check(rc == AXL_HASH_TABLE_NEW, "ins_vs_rep: first replace returns NEW");

    // replace with collision — OLD key (key_b1) should be freed, NEW key (key_b2) kept;
    // return value should be 0 (replaced)
    free_count = 0;
    rc = axl_hash_table_replace(t, key_b2, (void *)20);
    test_check(rc == AXL_HASH_TABLE_REPLACED, "ins_vs_rep: collision replace returns REPLACED");
    // Expect: key_destroy(key_b1) + value_destroy(old value)
    test_check(free_count == 2, "ins_vs_rep: replace collision frees old key + old value");

    got = axl_hash_table_lookup(t, "key_b");
    test_check(got == (void *)20, "ins_vs_rep: replace updated value");

    axl_hash_table_free(t);
    /* Reclaim test fixture keys (see comment above). */
    axl_free(key_b1);
    axl_free(key_b2);
}

static void
test_hash_steal_copy_keys(void)
{
    // steal on a copy_keys table should free the internal key copy
    // but NOT call value_destroy (there isn't one on new() tables,
    // but we verify no leak by checking the operation succeeds and
    // the entry is gone).

    AxlHashTable *t = axl_hash_table_new_str();

    axl_hash_table_insert(t, "keep", (void *)1);
    axl_hash_table_insert(t, "take", (void *)2);
    test_check(axl_hash_table_size(t) == 2, "steal_copy: size 2");

    // Steal — internal key copy freed, value returned to caller
    bool ok = axl_hash_table_steal(t, "take");
    test_check(ok, "steal_copy: returns true");
    test_check(axl_hash_table_size(t) == 1, "steal_copy: size 1");
    test_check(axl_hash_table_lookup(t, "take") == NULL, "steal_copy: entry gone");

    // Remaining entry unaffected
    test_check(axl_hash_table_lookup(t, "keep") == (void *)1, "steal_copy: keep intact");

    axl_hash_table_free(t);
}

// ---------------------------------------------------------------------------
// String Tests
// ---------------------------------------------------------------------------

static void
test_string_ascii(void)
{
    char *dup;
    char *dup_n;

    dup = axl_strdup("hello world");
    test_check(dup != NULL, "str8: dup non-NULL");
    if (dup != NULL) {
        test_check(axl_strcmp(dup, "hello world") == 0, "str8: dup matches");
        axl_free(dup);
    }

    dup = axl_strdup(NULL);
    test_check(dup == NULL, "str8: dup NULL returns NULL");

    dup_n = axl_strndup("content-type", 7);
    test_check(dup_n != NULL, "str8: dupN non-NULL");
    if (dup_n != NULL) {
        test_check(axl_strcmp(dup_n, "content") == 0, "str8: dupN truncates");
        axl_free(dup_n);
    }

    dup_n = axl_strndup("abc", 0);
    test_check(dup_n != NULL, "str8: dupN zero len");
    if (dup_n != NULL) {
        test_check(dup_n[0] == '\0', "str8: dupN zero len is empty");
        axl_free(dup_n);
    }
}

// ---------------------------------------------------------------------------
// Array Tests
// ---------------------------------------------------------------------------

static int
compare_uintn(const void *a, const void *b)
{
    size_t va = *(const size_t *)a;
    size_t vb = *(const size_t *)b;

    if (va < vb) { return -1; }
    if (va > vb) { return 1; }
    return 0;
}

static void
test_array(void)
{
    AxlArray *arr;
    size_t val;
    size_t *got;
    void *ptr;
    size_t unsorted[] = { 50, 10, 40, 20, 30 };
    size_t i;

    // Value mode
    arr = axl_array_new(sizeof(size_t));
    test_check(arr != NULL, "array: new");
    if (arr == NULL) {
        return;
    }

    val = 111;
    axl_array_append(arr, &val);
    val = 222;
    axl_array_append(arr, &val);
    val = 333;
    axl_array_append(arr, &val);

    test_check(axl_array_len(arr) == 3, "array: len is 3");

    got = axl_array_get(arr, 1);
    test_check(got != NULL && *got == 222, "array: get index 1");

    got = axl_array_get(arr, 5);
    test_check(got == NULL, "array: get out of range");

    // Clear
    axl_array_clear(arr);
    test_check(axl_array_len(arr) == 0, "array: clear resets len");

    // Sort
    for (i = 0; i < 5; i++) {
        axl_array_append(arr, &unsorted[i]);
    }
    axl_array_sort(arr, compare_uintn);
    got = axl_array_get(arr, 0);
    test_check(got != NULL && *got == 10, "array: sort first is 10");
    got = axl_array_get(arr, 4);
    test_check(got != NULL && *got == 50, "array: sort last is 50");

    axl_array_free(arr);

    // Pointer mode
    arr = axl_array_new(sizeof(void *));
    test_check(arr != NULL, "array: new ptr mode");
    if (arr == NULL) {
        return;
    }

    axl_array_append_ptr(arr, (void *)(size_t)0xDEAD);
    axl_array_append_ptr(arr, (void *)(size_t)0xBEEF);
    test_check(axl_array_len(arr) == 2, "array: ptr len is 2");

    ptr = axl_array_get_ptr(arr, 0);
    test_check(ptr == (void *)(size_t)0xDEAD, "array: getptr 0");
    ptr = axl_array_get_ptr(arr, 1);
    test_check(ptr == (void *)(size_t)0xBEEF, "array: getptr 1");

    axl_array_free(arr);
}

static int
compare_uintn_with_data(const void *a, const void *b, void *user_data)
{
    size_t va = *(const size_t *)a;
    size_t vb = *(const size_t *)b;
    int descending = *(int *)user_data;

    if (descending) {
        if (va > vb) { return -1; }
        if (va < vb) { return 1; }
        return 0;
    }

    if (va < vb) { return -1; }
    if (va > vb) { return 1; }
    return 0;
}

static void
test_array_extended(void)
{
    AxlArray *arr;
    size_t val;
    size_t *got;
    int descending;

    // -- remove_index: [10,20,30,40] remove index 1 -> [10,30,40] --
    arr = axl_array_new(sizeof(size_t));
    val = 10; axl_array_append(arr, &val);
    val = 20; axl_array_append(arr, &val);
    val = 30; axl_array_append(arr, &val);
    val = 40; axl_array_append(arr, &val);

    test_check(axl_array_remove_index(arr, 1) == AXL_OK, "array: remove_index ok");
    test_check(axl_array_len(arr) == 3, "array: remove_index len");
    got = axl_array_get(arr, 0);
    test_check(got != NULL && *got == 10, "array: remove_index [0]=10");
    got = axl_array_get(arr, 1);
    test_check(got != NULL && *got == 30, "array: remove_index [1]=30");
    got = axl_array_get(arr, 2);
    test_check(got != NULL && *got == 40, "array: remove_index [2]=40");
    test_check(axl_array_remove_index(arr, 5) == AXL_ERR, "array: remove_index oob");
    axl_array_free(arr);

    // -- remove_index_fast: [10,20,30,40] remove index 0 -> [40,20,30] --
    arr = axl_array_new(sizeof(size_t));
    val = 10; axl_array_append(arr, &val);
    val = 20; axl_array_append(arr, &val);
    val = 30; axl_array_append(arr, &val);
    val = 40; axl_array_append(arr, &val);

    test_check(axl_array_remove_index_fast(arr, 0) == AXL_OK, "array: remove_fast ok");
    test_check(axl_array_len(arr) == 3, "array: remove_fast len");
    got = axl_array_get(arr, 0);
    test_check(got != NULL && *got == 40, "array: remove_fast [0]=40");
    got = axl_array_get(arr, 1);
    test_check(got != NULL && *got == 20, "array: remove_fast [1]=20");
    got = axl_array_get(arr, 2);
    test_check(got != NULL && *got == 30, "array: remove_fast [2]=30");
    axl_array_free(arr);

    // -- remove_range: [10,20,30,40,50] remove range(1,2) -> [10,40,50] --
    arr = axl_array_new(sizeof(size_t));
    val = 10; axl_array_append(arr, &val);
    val = 20; axl_array_append(arr, &val);
    val = 30; axl_array_append(arr, &val);
    val = 40; axl_array_append(arr, &val);
    val = 50; axl_array_append(arr, &val);

    test_check(axl_array_remove_range(arr, 1, 2) == AXL_OK, "array: remove_range ok");
    test_check(axl_array_len(arr) == 3, "array: remove_range len");
    got = axl_array_get(arr, 0);
    test_check(got != NULL && *got == 10, "array: remove_range [0]=10");
    got = axl_array_get(arr, 1);
    test_check(got != NULL && *got == 40, "array: remove_range [1]=40");
    got = axl_array_get(arr, 2);
    test_check(got != NULL && *got == 50, "array: remove_range [2]=50");
    test_check(axl_array_remove_range(arr, 1, 5) == AXL_ERR, "array: remove_range oob");
    axl_array_free(arr);

    // -- set_size grow: 3 elements, set_size(5) --
    arr = axl_array_new(sizeof(size_t));
    val = 1; axl_array_append(arr, &val);
    val = 2; axl_array_append(arr, &val);
    val = 3; axl_array_append(arr, &val);

    test_check(axl_array_set_size(arr, 5) == AXL_OK, "array: set_size grow ok");
    test_check(axl_array_len(arr) == 5, "array: set_size grow len");
    got = axl_array_get(arr, 3);
    test_check(got != NULL && *got == 0, "array: set_size grow zero-init");

    // -- set_size shrink: set_size(2) --
    test_check(axl_array_set_size(arr, 2) == AXL_OK, "array: set_size shrink ok");
    test_check(axl_array_len(arr) == 2, "array: set_size shrink len");
    got = axl_array_get(arr, 0);
    test_check(got != NULL && *got == 1, "array: set_size shrink [0]=1");
    axl_array_free(arr);

    // -- sort_with_data: descending sort --
    arr = axl_array_new(sizeof(size_t));
    val = 30; axl_array_append(arr, &val);
    val = 10; axl_array_append(arr, &val);
    val = 20; axl_array_append(arr, &val);

    descending = 1;
    axl_array_sort_with_data(arr, compare_uintn_with_data, &descending);
    got = axl_array_get(arr, 0);
    test_check(got != NULL && *got == 30, "array: sort_with_data first=30");
    got = axl_array_get(arr, 2);
    test_check(got != NULL && *got == 10, "array: sort_with_data last=10");
    axl_array_free(arr);
}

// ---------------------------------------------------------------------------
// String Tests
// ---------------------------------------------------------------------------

static void
test_string(void)
{
    // UCS-2 higher-level utility tests removed — functions dropped from API.
    // UTF-8/UCS-2 conversion tested in axl-test-string.c (test_utf8_ucs2).
    (void)0;
}

// ---------------------------------------------------------------------------
// JSON Parse Tests
// ---------------------------------------------------------------------------

static void
test_json_parse(void)
{
    AxlJsonReader r;
    char str_buf[64];
    int64_t int_val;
    uint64_t uint_val;
    bool bool_val;
    bool ok;

    // Flat object
    const char *json = "{\"name\":\"devkit\",\"version\":42,\"debug\":true}";

    ok = axl_json_parse(json, axl_strlen(json), &r);
    test_check(ok, "json parse: valid");

    ok = axl_json_get_string(&r, "name", str_buf, sizeof(str_buf));
    test_check(ok && axl_strcmp(str_buf, "devkit") == 0, "json parse: get string");

    ok = axl_json_get_int(&r, "version", &int_val);
    test_check(ok && int_val == 42, "json parse: get int");

    ok = axl_json_get_uint(&r, "version", &uint_val);
    test_check(ok && uint_val == 42, "json parse: get uint");

    ok = axl_json_get_bool(&r, "debug", &bool_val);
    test_check(ok && bool_val == true, "json parse: get bool");

    // Missing key
    ok = axl_json_get_string(&r, "missing", str_buf, sizeof(str_buf));
    test_check(!ok, "json parse: missing key returns false");

    axl_json_free(&r);

    // Invalid JSON
    ok = axl_json_parse("not json", 8, &r);
    test_check(!ok, "json parse: invalid returns false");

    // One-shot convenience
    ok = axl_json_extract_string(json, axl_strlen(json), "name", str_buf, sizeof(str_buf));
    test_check(ok && axl_strcmp(str_buf, "devkit") == 0, "json parse: extract string");
}

// ---------------------------------------------------------------------------
// JSON Nested-Object Navigation Tests
// ---------------------------------------------------------------------------

static void
test_json_get_object(void)
{
    AxlJsonReader r;
    AxlJsonReader server;
    AxlJsonReader tls;
    AxlJsonReader sub;
    AxlJsonArrayIter it;
    AxlJsonReader elem;
    char    str_buf[64];
    int64_t int_val;
    bool    bool_val;
    bool    ok;
    int     n;

    const char *json =
        "{"
          "\"name\":\"devkit\","
          "\"server\":{"
            "\"host\":\"localhost\","
            "\"port\":8080,"
            "\"tls\":{\"enabled\":true,\"port\":443},"
            "\"aliases\":[\"a\",\"b\",\"c\"]"
          "},"
          "\"count\":3"
        "}";

    ok = axl_json_parse(json, axl_strlen(json), &r);
    test_check(ok, "json get_object: parse");

    // Navigate one level into a named nested object.
    ok = axl_json_get_object(&r, "server", &server);
    test_check(ok, "json get_object: nested object found");

    ok = axl_json_get_string(&server, "host", str_buf, sizeof(str_buf));
    test_check(ok && axl_strcmp(str_buf, "localhost") == 0,
               "json get_object: get string on sub-reader");

    ok = axl_json_get_int(&server, "port", &int_val);
    test_check(ok && int_val == 8080,
               "json get_object: get int on sub-reader");

    // Deep navigation: server -> tls -> {enabled, port}.
    ok = axl_json_get_object(&server, "tls", &tls);
    test_check(ok, "json get_object: deep nested object found");

    ok = axl_json_get_int(&tls, "port", &int_val);
    test_check(ok && int_val == 443,
               "json get_object: deep nested int (443)");

    ok = axl_json_get_bool(&tls, "enabled", &bool_val);
    test_check(ok && bool_val == true,
               "json get_object: deep nested bool");

    // Array nested inside a sub-object is reachable via array_begin.
    ok = axl_json_array_begin(&server, "aliases", &it);
    test_check(ok, "json get_object: array_begin on sub-reader");
    n = 0;
    while (axl_json_array_next(&it, &elem)) {
        n++;
    }
    test_check(n == 3, "json get_object: nested array iterates 3 elements");

    // axl_json_value_string reads each bare-string array element's value.
    ok = axl_json_array_begin(&server, "aliases", &it);
    test_check(ok, "json value_string: array_begin aliases");
    {
        const char *want[] = { "a", "b", "c" };
        int i = 0;
        bool all = true;
        while (axl_json_array_next(&it, &elem)) {
            char vbuf[16] = { 0 };
            if (!axl_json_value_string(&elem, vbuf, sizeof(vbuf))
                || i >= 3 || axl_strcmp(vbuf, want[i]) != 0) {
                all = false;
            }
            i++;
        }
        test_check(all && i == 3,
                   "json value_string: reads each string element a/b/c");
    }

    // Negative: value_string on an object reader (server) is not a string.
    test_check(!axl_json_value_string(&server, str_buf, sizeof(str_buf)),
               "json value_string: object reader returns false");

    // Negative: NULL args fail closed.
    test_check(!axl_json_value_string(NULL, str_buf, sizeof(str_buf)),
               "json value_string: NULL reader returns false");

    // Negative: missing key.
    ok = axl_json_get_object(&r, "missing", &sub);
    test_check(!ok, "json get_object: missing key returns false");

    // Negative: key maps to a scalar, not an object.
    ok = axl_json_get_object(&r, "name", &sub);
    test_check(!ok, "json get_object: scalar value returns false");

    // Negative: key maps to an array, not an object.
    ok = axl_json_get_object(&server, "aliases", &sub);
    test_check(!ok, "json get_object: array value returns false");

    // A parent sibling after the nested object is still reachable.
    ok = axl_json_get_int(&r, "count", &int_val);
    test_check(ok && int_val == 3,
               "json get_object: parent sibling after nested object");

    // The sub-reader borrows the parent's tokens: freeing it does not
    // release the parent's memory (no double-free) and leaves the parent
    // reader fully usable. (Per contract, callers shouldn't free borrowed
    // readers; this pins that doing so is at worst harmless.)
    axl_json_free(&server);
    ok = axl_json_get_string(&r, "name", str_buf, sizeof(str_buf));
    test_check(ok && axl_strcmp(str_buf, "devkit") == 0,
               "json get_object: parent usable after freeing borrowed sub-reader");
    ok = axl_json_get_object(&r, "server", &server) &&
         axl_json_get_int(&server, "port", &int_val);
    test_check(ok && int_val == 8080,
               "json get_object: parent re-navigable after borrowed free");

    axl_json_free(&r);

    // JSON5-parsed readers navigate identically.
    const char *j5 =
        "{ db: { url: 'pg://x', pool: { max: 16 } } }";
    ok = axl_json_parse_flags(j5, axl_strlen(j5),
                              AXL_JSON_PARSER_JSON5, &r);
    test_check(ok, "json get_object: JSON5 parse");

    ok = axl_json_get_object(&r, "db", &server) &&
         axl_json_get_object(&server, "pool", &tls);
    test_check(ok, "json get_object: JSON5 deep navigation");

    ok = axl_json_get_int(&tls, "max", &int_val);
    test_check(ok && int_val == 16, "json get_object: JSON5 deep int (16)");

    axl_json_free(&r);
}

// ---------------------------------------------------------------------------
// JSON5 Parse Tests
// ---------------------------------------------------------------------------

static void
test_json5_parse(void)
{
    AxlJsonReader r;
    char          str_buf[64];
    int64_t       int_val;
    bool          bool_val;
    bool          ok;

    // Comments + trailing commas + unquoted keys + single quotes + hex
    const char *j5 =
        "// header comment\n"
        "{\n"
        "  /* block comment */\n"
        "  name: 'devkit',\n"           // unquoted key + single-quoted string
        "  version: 42,\n"
        "  port: 0xCA2,\n"              // hex literal
        "  debug: true,\n"              // trailing comma below
        "}\n";

    ok = axl_json_parse_flags(j5, axl_strlen(j5),
                              AXL_JSON_PARSER_JSON5, &r);
    test_check(ok, "json5 parse: comments + trailing comma + unquoted keys");

    ok = axl_json_get_string(&r, "name", str_buf, sizeof(str_buf));
    test_check(ok && axl_strcmp(str_buf, "devkit") == 0,
               "json5 parse: single-quoted string value via unquoted key");

    ok = axl_json_get_int(&r, "version", &int_val);
    test_check(ok && int_val == 42, "json5 parse: decimal int");

    ok = axl_json_get_int(&r, "port", &int_val);
    test_check(ok && int_val == 0xCA2, "json5 parse: hex int (0xCA2)");

    ok = axl_json_get_bool(&r, "debug", &bool_val);
    test_check(ok && bool_val == true, "json5 parse: bool");

    axl_json_free(&r);

    // Trailing comma in array, hex with negative, x-escape in string
    const char *j5_arr =
        "{ items: [1, 2, 0x10, -0xFF,], greeting: \"hi\\x21\", }";
    ok = axl_json_parse_flags(j5_arr, axl_strlen(j5_arr),
                              AXL_JSON_PARSER_JSON5, &r);
    test_check(ok, "json5 parse: array trailing comma + signed hex + \\x escape");

    AxlJsonArrayIter it;
    ok = axl_json_array_begin(&r, "items", &it);
    test_check(ok, "json5 parse: array_begin on JSON5-parsed reader");

    int idx = 0;
    AxlJsonReader elem;
    while (axl_json_array_next(&it, &elem)) {
        idx++;
    }
    test_check(idx == 4, "json5 parse: array_next iterates all 4 elements");

    ok = axl_json_get_string(&r, "greeting", str_buf, sizeof(str_buf));
    test_check(ok && axl_strcmp(str_buf, "hi!") == 0,
               "json5 parse: \\x21 decoded to '!'");

    axl_json_free(&r);

    // Strict parser must STILL reject JSON5 input
    ok = axl_json_parse(j5, axl_strlen(j5), &r);
    test_check(!ok, "json5 parse: strict mode still rejects JSON5");

    // Default flags == strict
    ok = axl_json_parse_flags(j5, axl_strlen(j5),
                              AXL_JSON_PARSER_DEFAULT, &r);
    test_check(!ok, "json5 parse: AXL_JSON_PARSER_DEFAULT == strict");

    // Strict JSON parses correctly via the JSON5 path too (superset)
    const char *strict = "{\"a\":1,\"b\":[true,null]}";
    ok = axl_json_parse_flags(strict, axl_strlen(strict),
                              AXL_JSON_PARSER_JSON5, &r);
    test_check(ok, "json5 parse: accepts strict JSON unchanged");
    ok = axl_json_get_int(&r, "a", &int_val);
    test_check(ok && int_val == 1, "json5 parse: strict accessors work");
    axl_json_free(&r);

    // Malformed JSON5 fails (unterminated block comment)
    const char *bad = "{ /* never closed \n a: 1 }";
    ok = axl_json_parse_flags(bad, axl_strlen(bad),
                              AXL_JSON_PARSER_JSON5, &r);
    test_check(!ok, "json5 parse: unterminated block comment rejected");
}

// ---------------------------------------------------------------------------
// axl_json_load_file — round-trip via fs0:\axl_test_jload.tmp
// ---------------------------------------------------------------------------

static void
test_json_load_file(void)
{
    static const char  path[]    = "fs0:\\axl_test_jload.tmp";
    static const char  json[]    = "{\"name\":\"axl\",\"answer\":42}";
    AxlJsonReader      r          = { 0 };
    void              *raw        = NULL;
    size_t             raw_len    = 0;

    /* Stage the JSON file. */
    test_check(axl_file_set_contents(path, json, sizeof(json) - 1) == AXL_OK,
               "json_load_file: set_contents seeds the fixture");

    /* Successful load. */
    test_check(axl_json_load_file(path, &r, &raw, &raw_len),
               "json_load_file: load + parse succeeds");
    test_check(raw != NULL && raw_len == sizeof(json) - 1,
               "json_load_file: out_buf populated, out_len matches file size");

    char str_buf[16];
    test_check(axl_json_get_string(&r, "name", str_buf, sizeof(str_buf))
               && axl_strcmp(str_buf, "axl") == 0,
               "json_load_file: parsed reader extracts string");

    axl_json_free(&r);
    axl_free(raw);

    /* Failure path: missing file leaves out_buf NULL and returns false. */
    AxlJsonReader r2  = { 0 };
    void         *raw2 = (void *)0xdead;   /* sentinel; impl must overwrite */
    test_check(!axl_json_load_file("fs0:\\__definitely_missing__.tmp",
                                   &r2, &raw2, NULL),
               "json_load_file: missing file returns false");
    test_check(raw2 == NULL,
               "json_load_file: out_buf cleared on failure");
}

// ---------------------------------------------------------------------------
// JSON Build Tests
// ---------------------------------------------------------------------------

static void
test_json_build(void)
{
    AxlJsonWriter w;
    AxlJsonReader r;
    char str_buf[64];
    int64_t int_val;
    bool bool_val;

    // Build a simple object
    AxlString *out = axl_string_new(NULL);
    axl_json_writer_init(&w, out, AXL_JSON_WRITER_DEFAULT);
    axl_json_obj_begin(&w);
    axl_json_kv_str(&w, "name", "devkit");
    axl_json_kv_int(&w, "version", 42);
    axl_json_kv_bool(&w, "debug", true);
    axl_json_kv_null(&w, "extra");
    axl_json_obj_end(&w);
    size_t len = axl_json_writer_finish(&w);

    test_check(len > 0, "json build: non-empty");
    test_check(!axl_json_writer_error(&w), "json build: no error");

    // Round-trip: parse what we built
    const char *built = axl_string_str(out);
    test_check(axl_json_parse(built, len, &r), "json build: round-trip parse");
    test_check(axl_json_get_string(&r, "name", str_buf, sizeof(str_buf))
               && axl_strcmp(str_buf, "devkit") == 0,
               "json build: round-trip name");
    test_check(axl_json_get_int(&r, "version", &int_val) && int_val == 42,
               "json build: round-trip version");
    test_check(axl_json_get_bool(&r, "debug", &bool_val) && bool_val == true,
               "json build: round-trip debug");

    axl_json_free(&r);
    axl_string_free(out);

    // Structural-misuse test: emit a key inside an array (should set sticky error)
    out = axl_string_new(NULL);
    axl_json_writer_init(&w, out, AXL_JSON_WRITER_DEFAULT);
    axl_json_arr_begin(&w);
    axl_json_key(&w, "bad");        // illegal: key inside array
    axl_json_writer_finish(&w);
    test_check(axl_json_writer_error(&w), "json build: misuse sets error");
    axl_string_free(out);

    // Nested: array of strings, object with named array
    out = axl_string_new(NULL);
    axl_json_writer_init(&w, out, AXL_JSON_WRITER_DEFAULT);
    axl_json_obj_begin(&w);
        axl_json_key(&w, "items");
        axl_json_arr_begin(&w);
            axl_json_str(&w, "one");
            axl_json_str(&w, "two");
        axl_json_arr_end(&w);
    axl_json_obj_end(&w);
    len = axl_json_writer_finish(&w);
    test_check(len > 0 && !axl_json_writer_error(&w),
               "json build: nested object with array");
    test_check(axl_strcmp(axl_string_str(out),
                          "{\"items\":[\"one\",\"two\"]}") == 0,
               "json build: nested compact output exact");
    axl_string_free(out);

    // Pretty mode
    out = axl_string_new(NULL);
    axl_json_writer_init(&w, out, AXL_JSON_WRITER_PRETTY);
    axl_json_obj_begin(&w);
    axl_json_kv_str(&w, "name", "AXL");
    axl_json_kv_uint(&w, "version", 1);
    axl_json_obj_end(&w);
    axl_json_writer_finish(&w);
    test_check(!axl_json_writer_error(&w), "json build: pretty no error");
    test_check(axl_strcmp(axl_string_str(out),
                          "{\n  \"name\": \"AXL\",\n  \"version\": 1\n}") == 0,
               "json build: pretty exact output");
    axl_string_free(out);

    // Bridge: write_token splices a parsed sub-document
    const char *src = "{\"a\":1,\"b\":[2,3]}";
    test_check(axl_json_parse(src, axl_strlen(src), &r), "json bridge: parse src");
    out = axl_string_new(NULL);
    axl_json_writer_init(&w, out, AXL_JSON_WRITER_DEFAULT);
    axl_json_obj_begin(&w);
        axl_json_key(&w, "wrapped");
        axl_json_write_token(&w, &r, 0);   // splice entire src doc
    axl_json_obj_end(&w);
    axl_json_writer_finish(&w);
    test_check(!axl_json_writer_error(&w), "json bridge: no error");
    test_check(axl_strcmp(axl_string_str(out),
                          "{\"wrapped\":{\"a\":1,\"b\":[2,3]}}") == 0,
               "json bridge: round-trip exact output");
    axl_json_free(&r);
    axl_string_free(out);

    // Bridge: \uXXXX escapes round-trip verbatim
    const char *uesc = "{\"k\":\"a\\u00e9b\"}";
    test_check(axl_json_parse(uesc, axl_strlen(uesc), &r),
               "json bridge: parse escape src");
    out = axl_string_new(NULL);
    axl_json_writer_init(&w, out, AXL_JSON_WRITER_DEFAULT);
    axl_json_write_token(&w, &r, 0);
    axl_json_writer_finish(&w);
    test_check(!axl_json_writer_error(&w),
               "json bridge: escape no error");
    test_check(axl_strcmp(axl_string_str(out), uesc) == 0,
               "json bridge: \\uXXXX preserved verbatim");
    axl_json_free(&r);
    axl_string_free(out);

    // Top-level atom rejection (matches parser's bare-primitive rejection)
    out = axl_string_new(NULL);
    axl_json_writer_init(&w, out, AXL_JSON_WRITER_DEFAULT);
    axl_json_str(&w, "lonely");
    test_check(axl_json_writer_error(&w),
               "json build: top-level atom is rejected");
    axl_string_free(out);

    // Empty containers in pretty mode emit no internal whitespace
    out = axl_string_new(NULL);
    axl_json_writer_init(&w, out, AXL_JSON_WRITER_PRETTY);
    axl_json_obj_begin(&w);
    axl_json_obj_end(&w);
    axl_json_writer_finish(&w);
    test_check(axl_strcmp(axl_string_str(out), "{}") == 0,
               "json build: pretty empty object is {}");
    axl_string_free(out);

    out = axl_string_new(NULL);
    axl_json_writer_init(&w, out, AXL_JSON_WRITER_PRETTY);
    axl_json_arr_begin(&w);
    axl_json_arr_end(&w);
    axl_json_writer_finish(&w);
    test_check(axl_strcmp(axl_string_str(out), "[]") == 0,
               "json build: pretty empty array is []");
    axl_string_free(out);

    // Numeric boundary: INT64_MIN
    out = axl_string_new(NULL);
    axl_json_writer_init(&w, out, AXL_JSON_WRITER_DEFAULT);
    axl_json_obj_begin(&w);
    axl_json_kv_int(&w, "min", INT64_MIN);
    axl_json_obj_end(&w);
    axl_json_writer_finish(&w);
    test_check(axl_strcmp(axl_string_str(out),
                          "{\"min\":-9223372036854775808}") == 0,
               "json build: INT64_MIN renders correctly");
    axl_string_free(out);

    // Numeric boundary: UINT64_MAX
    out = axl_string_new(NULL);
    axl_json_writer_init(&w, out, AXL_JSON_WRITER_DEFAULT);
    axl_json_obj_begin(&w);
    axl_json_kv_uint(&w, "max", UINT64_MAX);
    axl_json_obj_end(&w);
    axl_json_writer_finish(&w);
    test_check(axl_strcmp(axl_string_str(out),
                          "{\"max\":18446744073709551615}") == 0,
               "json build: UINT64_MAX renders correctly");
    axl_string_free(out);

    // Writer in error state no-ops subsequent calls
    out = axl_string_new(NULL);
    axl_json_writer_init(&w, out, AXL_JSON_WRITER_DEFAULT);
    axl_json_arr_begin(&w);
    axl_json_key(&w, "bad");        // sets sticky error
    size_t len_at_error = axl_string_len(out);
    axl_json_str(&w, "ignored");    // must be a no-op
    axl_json_arr_end(&w);           // must be a no-op
    test_check(axl_json_writer_error(&w),
               "json build: error sticks");
    test_check(axl_string_len(out) == len_at_error,
               "json build: post-error calls are no-ops");
    axl_string_free(out);

    // axl_json_raw(NULL) sets sticky error
    out = axl_string_new(NULL);
    axl_json_writer_init(&w, out, AXL_JSON_WRITER_DEFAULT);
    axl_json_obj_begin(&w);
    axl_json_key(&w, "x");
    axl_json_raw(&w, NULL);
    test_check(axl_json_writer_error(&w),
               "json build: raw(NULL) sets error");
    axl_string_free(out);

    // axl_json_kv_strn for non-NUL-terminated values
    out = axl_string_new(NULL);
    axl_json_writer_init(&w, out, AXL_JSON_WRITER_DEFAULT);
    const char *not_terminated = "abcXYZ";   // pretend len-3 slice
    axl_json_obj_begin(&w);
    axl_json_kv_strn(&w, "k", not_terminated, 3);
    axl_json_obj_end(&w);
    axl_json_writer_finish(&w);
    test_check(axl_strcmp(axl_string_str(out), "{\"k\":\"abc\"}") == 0,
               "json build: kv_strn slice is exact");
    axl_string_free(out);
}

// ---------------------------------------------------------------------------
// JSON5 Build Tests — comments, trailing commas, and round-trip with reader
// ---------------------------------------------------------------------------

static void
test_json5_build(void)
{
    AxlJsonWriter w;
    AxlJsonReader r;
    AxlString    *out;

    /* Trailing commas, compact */
    out = axl_string_new(NULL);
    axl_json_writer_init(&w, out, AXL_JSON_WRITER_TRAILING_COMMAS);
    axl_json_obj_begin(&w);
    axl_json_kv_int(&w, "a", 1);
    axl_json_kv_int(&w, "b", 2);
    axl_json_obj_end(&w);
    axl_json_writer_finish(&w);
    test_check(!axl_json_writer_error(&w), "json5 build: trailing-comma no error");
    test_check(axl_strcmp(axl_string_str(out), "{\"a\":1,\"b\":2,}") == 0,
               "json5 build: trailing comma in compact object");
    /* And the JSON5 reader accepts it. */
    test_check(axl_json_parse_flags(axl_string_str(out),
                                    axl_string_len(out),
                                    AXL_JSON_PARSER_JSON5, &r),
               "json5 build: trailing-comma output round-trips through JSON5 reader");
    axl_json_free(&r);
    axl_string_free(out);

    /* Trailing commas, pretty */
    out = axl_string_new(NULL);
    axl_json_writer_init(&w, out,
        AXL_JSON_WRITER_PRETTY | AXL_JSON_WRITER_TRAILING_COMMAS);
    axl_json_arr_begin(&w);
    axl_json_int(&w, 1);
    axl_json_int(&w, 2);
    axl_json_arr_end(&w);
    axl_json_writer_finish(&w);
    test_check(axl_strcmp(axl_string_str(out), "[\n  1,\n  2,\n]") == 0,
               "json5 build: pretty array trailing comma");
    axl_string_free(out);

    /* Comment between values, pretty */
    out = axl_string_new(NULL);
    axl_json_writer_init(&w, out, AXL_JSON_WRITER_PRETTY);
    axl_json_obj_begin(&w);
    axl_json_kv_str(&w, "name", "AXL");
    axl_json_comment(&w, "version comes from build system");
    axl_json_kv_int(&w, "version", 1);
    axl_json_obj_end(&w);
    axl_json_writer_finish(&w);
    test_check(!axl_json_writer_error(&w), "json5 build: comment no error");
    const char *expected_pretty =
        "{\n"
        "  \"name\": \"AXL\",\n"
        "  // version comes from build system\n"
        "  \"version\": 1\n"
        "}";
    test_check(axl_strcmp(axl_string_str(out), expected_pretty) == 0,
               "json5 build: pretty comment between values exact output");
    /* JSON5 reader accepts it. */
    test_check(axl_json_parse_flags(axl_string_str(out),
                                    axl_string_len(out),
                                    AXL_JSON_PARSER_JSON5, &r),
               "json5 build: pretty comment output round-trips");
    axl_json_free(&r);
    axl_string_free(out);

    /* Comment in compact mode emits inline / * ... * / */
    out = axl_string_new(NULL);
    axl_json_writer_init(&w, out, AXL_JSON_WRITER_DEFAULT);
    axl_json_obj_begin(&w);
    axl_json_comment(&w, "header");
    axl_json_kv_int(&w, "x", 7);
    axl_json_obj_end(&w);
    axl_json_writer_finish(&w);
    test_check(axl_strcmp(axl_string_str(out), "{/* header */\"x\":7}") == 0,
               "json5 build: compact inline comment");
    axl_string_free(out);

    /* Comment-as-last-item must still get the closing brace on its own line
       in pretty mode. */
    out = axl_string_new(NULL);
    axl_json_writer_init(&w, out, AXL_JSON_WRITER_PRETTY);
    axl_json_obj_begin(&w);
    axl_json_kv_int(&w, "x", 1);
    axl_json_comment(&w, "trailing note");
    axl_json_obj_end(&w);
    axl_json_writer_finish(&w);
    test_check(axl_strcmp(axl_string_str(out),
                          "{\n  \"x\": 1,\n  // trailing note\n}") == 0,
               "json5 build: trailing comment dedents close brace");
    axl_string_free(out);

    /* Comment sanitization: embedded newline truncates; embedded close-comment
       sequence in compact mode is split. */
    out = axl_string_new(NULL);
    axl_json_writer_init(&w, out, AXL_JSON_WRITER_DEFAULT);
    axl_json_obj_begin(&w);
    axl_json_comment(&w, "a*/b");           // close-comment in middle
    axl_json_kv_int(&w, "k", 1);
    axl_json_obj_end(&w);
    axl_json_writer_finish(&w);
    test_check(axl_strcmp(axl_string_str(out),
                          "{/* a* /b */\"k\":1}") == 0,
               "json5 build: compact comment sanitizes embedded close-comment");
    axl_string_free(out);
}

// ---------------------------------------------------------------------------
// JSON Print Test
// ---------------------------------------------------------------------------

static void
test_json_print(void)
{
    const char *json = "{\"key\":\"value\",\"num\":123}";

    // Visual test — just verify it doesn't crash
    axl_printf("\njson pretty-print output:\n");
    axl_json_console_print(json, axl_strlen(json));
    test_pass("json print: no crash");
}

// ---------------------------------------------------------------------------
// Singly-Linked List Tests
// ---------------------------------------------------------------------------

static int
int_compare(const void *a, const void *b)
{
    intptr_t ia = (intptr_t)a;
    intptr_t ib = (intptr_t)b;
    return (ia > ib) - (ia < ib);
}

static size_t slist_foreach_sum;

static void
slist_sum_func(void *data, void *user_data)
{
    (void)user_data;
    slist_foreach_sum += (intptr_t)data;
}

static size_t slist_destroy_count;

static void
slist_destroy_counter(void *data)
{
    (void)data;
    slist_destroy_count++;
}

static void
test_slist(void)
{
    AxlSList *list = NULL;

    /* prepend 3 items: 3 -> 2 -> 1 */
    list = axl_slist_prepend(list, (void *)1);
    list = axl_slist_prepend(list, (void *)2);
    list = axl_slist_prepend(list, (void *)3);
    test_check(axl_slist_length(list) == 3, "slist: prepend length 3");
    test_check((intptr_t)list->data == 3, "slist: prepend head is 3");

    /* append */
    list = axl_slist_append(list, (void *)4);
    test_check((intptr_t)axl_slist_last(list)->data == 4, "slist: append tail is 4");

    /* nth_data */
    test_check((intptr_t)axl_slist_nth_data(list, 1) == 2, "slist: nth_data(1) is 2");

    /* remove */
    list = axl_slist_remove(list, (void *)2);
    test_check(axl_slist_length(list) == 3, "slist: remove length 3");
    test_check(axl_slist_find(list, (void *)2) == NULL, "slist: removed item not found");

    /* reverse: 4 -> 1 -> 3  =>  3 -> 1 -> 4 */
    list = axl_slist_reverse(list);
    test_check((intptr_t)list->data == 4, "slist: reverse head is 4");

    /* sort */
    list = axl_slist_sort(list, int_compare);
    test_check((intptr_t)list->data == 1, "slist: sort head is 1");
    test_check((intptr_t)axl_slist_last(list)->data == 4, "slist: sort tail is 4");

    /* find_custom */
    test_check(axl_slist_find_custom(list, (void *)3, int_compare) != NULL,
               "slist: find_custom 3");

    /* foreach */
    slist_foreach_sum = 0;
    axl_slist_foreach(list, slist_sum_func, NULL);
    test_check(slist_foreach_sum == 8, "slist: foreach sum is 8");

    /* free_full */
    slist_destroy_count = 0;
    axl_slist_free_full(list, slist_destroy_counter);
    test_check(slist_destroy_count == 3, "slist: free_full called 3 times");
}

// ---------------------------------------------------------------------------
// Doubly-Linked List Tests
// ---------------------------------------------------------------------------

static void
test_list(void)
{
    AxlList *list = NULL;

    /* append 3 items: 1 -> 2 -> 3 */
    list = axl_list_append(list, (void *)1);
    list = axl_list_append(list, (void *)2);
    list = axl_list_append(list, (void *)3);
    test_check(axl_list_length(list) == 3, "list: append length 3");
    test_check((intptr_t)list->data == 1, "list: append head is 1");

    /* bidirectional links */
    AxlList *last = axl_list_last(list);
    test_check((intptr_t)last->data == 3, "list: last is 3");
    test_check(last->prev != NULL && (intptr_t)last->prev->data == 2,
               "list: last->prev is 2");

    /* first from middle */
    AxlList *mid = axl_list_nth(list, 1);
    test_check(axl_list_first(mid) == list, "list: first from middle");

    /* insert_sorted into empty, then build sorted list */
    AxlList *sorted = NULL;
    sorted = axl_list_insert_sorted(sorted, (void *)3, int_compare);
    sorted = axl_list_insert_sorted(sorted, (void *)1, int_compare);
    sorted = axl_list_insert_sorted(sorted, (void *)2, int_compare);
    test_check((intptr_t)sorted->data == 1, "list: insert_sorted head 1");
    test_check((intptr_t)axl_list_nth_data(sorted, 1) == 2,
               "list: insert_sorted mid 2");
    test_check((intptr_t)axl_list_last(sorted)->data == 3,
               "list: insert_sorted tail 3");
    axl_list_free(sorted);

    /* remove from middle */
    list = axl_list_remove(list, (void *)2);
    test_check(axl_list_length(list) == 2, "list: remove length 2");
    test_check(list->next != NULL && list->next->prev == list,
               "list: remove preserves prev link");

    /* sort */
    list = axl_list_prepend(list, (void *)5);
    list = axl_list_sort(list, int_compare);
    test_check((intptr_t)list->data == 1, "list: sort head is 1");
    test_check((intptr_t)axl_list_last(list)->data == 5,
               "list: sort tail is 5");

    /* copy */
    AxlList *copy = axl_list_copy(list);
    test_check(axl_list_length(copy) == axl_list_length(list),
               "list: copy same length");
    test_check(copy != list, "list: copy is distinct");
    axl_list_free(copy);

    axl_list_free(list);
}

// ---------------------------------------------------------------------------
// Queue Tests
// ---------------------------------------------------------------------------

static void
test_queue(void)
{
    AxlQueue *q = axl_queue_new();
    test_check(q != NULL, "queue: new");
    if (q == NULL) {
        return;
    }

    test_check(axl_queue_is_empty(q), "queue: initially empty");

    /* push_tail (FIFO): 1, 2, 3 */
    axl_queue_push_tail(q, (void *)1);
    axl_queue_push_tail(q, (void *)2);
    axl_queue_push_tail(q, (void *)3);
    test_check(axl_queue_get_length(q) == 3, "queue: length 3");

    /* peek */
    test_check((intptr_t)axl_queue_peek_head(q) == 1, "queue: peek_head is 1");
    test_check((intptr_t)axl_queue_peek_tail(q) == 3, "queue: peek_tail is 3");
    test_check((intptr_t)axl_queue_peek_nth(q, 1) == 2, "queue: peek_nth(1) is 2");

    /* pop_head (FIFO order) */
    test_check((intptr_t)axl_queue_pop_head(q) == 1, "queue: pop_head is 1");
    test_check(axl_queue_get_length(q) == 2, "queue: length after pop is 2");

    /* pop_tail */
    test_check((intptr_t)axl_queue_pop_tail(q) == 3, "queue: pop_tail is 3");

    /* push_head (stack behavior) */
    axl_queue_push_head(q, (void *)10);
    test_check((intptr_t)axl_queue_peek_head(q) == 10, "queue: push_head is 10");

    /* drain */
    axl_queue_pop_head(q);
    axl_queue_pop_head(q);
    test_check(axl_queue_is_empty(q), "queue: empty after drain");

    /* sort */
    axl_queue_push_tail(q, (void *)3);
    axl_queue_push_tail(q, (void *)1);
    axl_queue_push_tail(q, (void *)2);
    axl_queue_sort(q, int_compare);
    test_check((intptr_t)axl_queue_peek_head(q) == 1, "queue: sort head is 1");
    test_check((intptr_t)axl_queue_peek_tail(q) == 3, "queue: sort tail is 3");

    /* stack-allocated init */
    AxlQueue sq = AXL_QUEUE_INIT;
    axl_queue_push_tail(&sq, (void *)42);
    test_check((intptr_t)axl_queue_pop_head(&sq) == 42,
               "queue: stack-allocated init works");
    axl_queue_clear(&sq);

    axl_queue_free(q);
}

// ---------------------------------------------------------------------------
// Extended List Tests
// ---------------------------------------------------------------------------

static int
int_compare_data(const void *a, const void *b, void *user_data)
{
    intptr_t offset = (intptr_t)user_data;
    intptr_t ia = (intptr_t)a + offset;
    intptr_t ib = (intptr_t)b + offset;
    return (ia > ib) - (ia < ib);
}

static void *
copy_with_offset(const void *src, void *user_data)
{
    intptr_t offset = (intptr_t)user_data;
    return (void *)((intptr_t)src + offset);
}

static void
test_list_extended(void)
{
    AxlList *list = NULL;
    AxlList *node;

    /* Build: 1 -> 2 -> 3 */
    list = axl_list_append(list, (void *)1);
    list = axl_list_append(list, (void *)2);
    list = axl_list_append(list, (void *)3);

    /* insert_before: insert 10 before node with data==2 */
    node = axl_list_find(list, (void *)2);
    list = axl_list_insert_before(list, node, (void *)10);
    test_check(axl_list_length(list) == 4, "list_ext: insert_before length 4");
    test_check((intptr_t)axl_list_nth_data(list, 1) == 10,
               "list_ext: insert_before placed at index 1");

    /* insert_after: insert 20 after node with data==2 */
    node = axl_list_find(list, (void *)2);
    list = axl_list_insert_after(list, node, (void *)20);
    test_check(axl_list_length(list) == 5, "list_ext: insert_after length 5");
    /* List is now: 1 -> 10 -> 2 -> 20 -> 3 */
    test_check((intptr_t)axl_list_nth_data(list, 3) == 20,
               "list_ext: insert_after placed at index 3");

    axl_list_free(list);

    /* remove_all: list with duplicates */
    list = NULL;
    list = axl_list_append(list, (void *)5);
    list = axl_list_append(list, (void *)3);
    list = axl_list_append(list, (void *)5);
    list = axl_list_append(list, (void *)7);
    list = axl_list_append(list, (void *)5);
    list = axl_list_remove_all(list, (void *)5);
    test_check(axl_list_length(list) == 2, "list_ext: remove_all length 2");
    test_check(axl_list_find(list, (void *)5) == NULL,
               "list_ext: remove_all no 5 remaining");
    axl_list_free(list);

    /* remove_link: unlink node, verify list intact */
    list = NULL;
    list = axl_list_append(list, (void *)1);
    list = axl_list_append(list, (void *)2);
    list = axl_list_append(list, (void *)3);
    node = axl_list_find(list, (void *)2);
    list = axl_list_remove_link(list, node);
    test_check(axl_list_length(list) == 2, "list_ext: remove_link length 2");
    test_check((intptr_t)node->data == 2, "list_ext: remove_link node data intact");
    axl_free(node);
    axl_list_free(list);

    /* sort_with_data: context sort with offset 0 */
    list = NULL;
    list = axl_list_append(list, (void *)3);
    list = axl_list_append(list, (void *)1);
    list = axl_list_append(list, (void *)2);
    list = axl_list_sort_with_data(list, int_compare_data, (void *)0);
    test_check((intptr_t)list->data == 1, "list_ext: sort_with_data head 1");
    test_check((intptr_t)axl_list_last(list)->data == 3,
               "list_ext: sort_with_data tail 3");
    axl_list_free(list);

    /* copy_deep: deep copy with +10 offset */
    list = NULL;
    list = axl_list_append(list, (void *)1);
    list = axl_list_append(list, (void *)2);
    list = axl_list_append(list, (void *)3);
    AxlList *deep = axl_list_copy_deep(list, copy_with_offset, (void *)10);
    test_check(axl_list_length(deep) == 3, "list_ext: copy_deep length 3");
    test_check((intptr_t)deep->data == 11, "list_ext: copy_deep head 11");
    test_check((intptr_t)axl_list_last(deep)->data == 13,
               "list_ext: copy_deep tail 13");
    axl_list_free(deep);
    axl_list_free(list);
}

// ---------------------------------------------------------------------------
// Extended SList Tests
// ---------------------------------------------------------------------------

static void
test_slist_extended(void)
{
    AxlSList *list = NULL;
    AxlSList *node;

    /* Build: 1 -> 2 -> 3 */
    list = axl_slist_append(list, (void *)1);
    list = axl_slist_append(list, (void *)2);
    list = axl_slist_append(list, (void *)3);

    /* insert_before: insert 10 before node with data==2 */
    node = axl_slist_find(list, (void *)2);
    list = axl_slist_insert_before(list, node, (void *)10);
    test_check(axl_slist_length(list) == 4, "slist_ext: insert_before length 4");
    test_check((intptr_t)axl_slist_nth_data(list, 1) == 10,
               "slist_ext: insert_before placed at index 1");
    axl_slist_free(list);

    /* remove_all: remove duplicates */
    list = NULL;
    list = axl_slist_append(list, (void *)5);
    list = axl_slist_append(list, (void *)3);
    list = axl_slist_append(list, (void *)5);
    list = axl_slist_append(list, (void *)7);
    list = axl_slist_append(list, (void *)5);
    list = axl_slist_remove_all(list, (void *)5);
    test_check(axl_slist_length(list) == 2, "slist_ext: remove_all length 2");
    test_check(axl_slist_find(list, (void *)5) == NULL,
               "slist_ext: remove_all no 5 remaining");
    axl_slist_free(list);

    /* remove_link: unlink */
    list = NULL;
    list = axl_slist_append(list, (void *)1);
    list = axl_slist_append(list, (void *)2);
    list = axl_slist_append(list, (void *)3);
    node = axl_slist_find(list, (void *)2);
    list = axl_slist_remove_link(list, node);
    test_check(axl_slist_length(list) == 2, "slist_ext: remove_link length 2");
    test_check((intptr_t)node->data == 2, "slist_ext: remove_link node data intact");
    axl_free(node);
    axl_slist_free(list);

    /* sort_with_data */
    list = NULL;
    list = axl_slist_append(list, (void *)3);
    list = axl_slist_append(list, (void *)1);
    list = axl_slist_append(list, (void *)2);
    list = axl_slist_sort_with_data(list, int_compare_data, (void *)0);
    test_check((intptr_t)list->data == 1, "slist_ext: sort_with_data head 1");
    test_check((intptr_t)axl_slist_last(list)->data == 3,
               "slist_ext: sort_with_data tail 3");
    axl_slist_free(list);

    /* copy_deep: +10 offset */
    list = NULL;
    list = axl_slist_append(list, (void *)1);
    list = axl_slist_append(list, (void *)2);
    list = axl_slist_append(list, (void *)3);
    AxlSList *deep = axl_slist_copy_deep(list, copy_with_offset, (void *)10);
    test_check(axl_slist_length(deep) == 3, "slist_ext: copy_deep length 3");
    test_check((intptr_t)deep->data == 11, "slist_ext: copy_deep head 11");
    test_check((intptr_t)axl_slist_last(deep)->data == 13,
               "slist_ext: copy_deep tail 13");
    axl_slist_free(deep);
    axl_slist_free(list);
}

// ---------------------------------------------------------------------------
// Extended Queue Tests
// ---------------------------------------------------------------------------

static void
test_queue_extended(void)
{
    AxlQueue *q = axl_queue_new();
    if (q == NULL) {
        test_check(false, "queue_ext: new");
        return;
    }

    /* Push: 1, 2, 3, 2, 5 */
    axl_queue_push_tail(q, (void *)1);
    axl_queue_push_tail(q, (void *)2);
    axl_queue_push_tail(q, (void *)3);
    axl_queue_push_tail(q, (void *)2);
    axl_queue_push_tail(q, (void *)5);

    /* find: present */
    test_check(axl_queue_find(q, (void *)3) != NULL, "queue_ext: find present");

    /* find: absent */
    test_check(axl_queue_find(q, (void *)99) == NULL, "queue_ext: find absent");

    /* find_custom */
    test_check(axl_queue_find_custom(q, (void *)5, int_compare) != NULL,
               "queue_ext: find_custom present");

    /* remove: removes first match of 2, length drops by 1 */
    test_check(axl_queue_remove(q, (void *)2) == true,
               "queue_ext: remove returns true");
    test_check(axl_queue_get_length(q) == 4, "queue_ext: remove length 4");

    /* remove_all: remove all remaining 2s (one left), returns count */
    size_t removed = axl_queue_remove_all(q, (void *)2);
    test_check(removed == 1, "queue_ext: remove_all count 1");
    test_check(axl_queue_get_length(q) == 3, "queue_ext: remove_all length 3");
    test_check(axl_queue_find(q, (void *)2) == NULL,
               "queue_ext: remove_all no 2 remaining");

    axl_queue_free(q);
}

// ---------------------------------------------------------------------------
// String Split/Join/Strip Tests
// ---------------------------------------------------------------------------

static void
test_str_split(void)
{
    char **parts;

    /* Basic split */
    parts = axl_strsplit("a,b,c", ',');
    test_check(parts != NULL, "strsplit: non-NULL");
    if (parts != NULL) {
        test_check(axl_strcmp(parts[0], "a") == 0, "strsplit: [0] is 'a'");
        test_check(axl_strcmp(parts[1], "b") == 0, "strsplit: [1] is 'b'");
        test_check(axl_strcmp(parts[2], "c") == 0, "strsplit: [2] is 'c'");
        test_check(parts[3] == NULL, "strsplit: [3] is NULL");
        axl_strfreev(parts);
    }

    /* Single element (no delimiter) */
    parts = axl_strsplit("hello", ',');
    test_check(parts != NULL && axl_strcmp(parts[0], "hello") == 0,
               "strsplit: single element");
    test_check(parts != NULL && parts[1] == NULL,
               "strsplit: single element NULL-terminated");
    axl_strfreev(parts);

    /* Empty segments */
    parts = axl_strsplit(",a,,b,", ',');
    test_check(parts != NULL && parts[0] != NULL && parts[0][0] == '\0',
               "strsplit: leading empty segment");
    axl_strfreev(parts);
}

static void
test_str_join(void)
{
    char *result;
    const char *arr[] = { "one", "two", "three", NULL };

    result = axl_strjoin(", ", arr);
    test_check(result != NULL && axl_strcmp(result, "one, two, three") == 0,
               "strjoin: comma-space separator");
    axl_free(result);

    /* No separator */
    result = axl_strjoin("", arr);
    test_check(result != NULL && axl_strcmp(result, "onetwothree") == 0,
               "strjoin: empty separator");
    axl_free(result);

    /* Single element */
    const char *single[] = { "solo", NULL };
    result = axl_strjoin(",", single);
    test_check(result != NULL && axl_strcmp(result, "solo") == 0,
               "strjoin: single element");
    axl_free(result);

    /* axl_strjoinv: argv-shape (count + array, no NULL terminator). */
    const char *const argv[] = { "one", "two", "three" };
    result = axl_strjoinv(", ", 3, argv);
    test_check(result != NULL && axl_strcmp(result, "one, two, three") == 0,
               "strjoinv: comma-space separator");
    axl_free(result);

    /* Single element via count=1 */
    result = axl_strjoinv(",", 1, argv);
    test_check(result != NULL && axl_strcmp(result, "one") == 0,
               "strjoinv: single element");
    axl_free(result);

    /* Empty separator */
    result = axl_strjoinv("", 3, argv);
    test_check(result != NULL && axl_strcmp(result, "onetwothree") == 0,
               "strjoinv: empty separator");
    axl_free(result);

    /* count == 0 → allocated empty string (NULL argv). */
    result = axl_strjoinv(",", 0, NULL);
    test_check(result != NULL && axl_strcmp(result, "") == 0,
               "strjoinv: count=0 NULL argv returns empty allocated string");
    axl_free(result);

    /* count == 0 → empty string even when argv is non-NULL. */
    result = axl_strjoinv(",", 0, argv);
    test_check(result != NULL && axl_strcmp(result, "") == 0,
               "strjoinv: count=0 non-NULL argv returns empty allocated string");
    axl_free(result);

    /* NULL separator behaves like empty separator (matches strjoin). */
    result = axl_strjoinv(NULL, 3, argv);
    test_check(result != NULL && axl_strcmp(result, "onetwothree") == 0,
               "strjoinv: NULL separator behaves like empty");
    axl_free(result);
}

static void
test_str_strip(void)
{
    char buf[64];

    /* Leading + trailing */
    axl_strlcpy(buf, "  hello  ", sizeof(buf));
    axl_strstrip(buf);
    test_check(axl_strcmp(buf, "hello") == 0, "strstrip: both sides");

    /* Leading only */
    axl_strlcpy(buf, "\t\nworld", sizeof(buf));
    axl_strstrip(buf);
    test_check(axl_strcmp(buf, "world") == 0, "strstrip: leading");

    /* Trailing only */
    axl_strlcpy(buf, "test\r\n", sizeof(buf));
    axl_strstrip(buf);
    test_check(axl_strcmp(buf, "test") == 0, "strstrip: trailing");

    /* All whitespace */
    axl_strlcpy(buf, "   ", sizeof(buf));
    axl_strstrip(buf);
    test_check(buf[0] == '\0', "strstrip: all whitespace -> empty");

    /* No whitespace */
    axl_strlcpy(buf, "clean", sizeof(buf));
    axl_strstrip(buf);
    test_check(axl_strcmp(buf, "clean") == 0, "strstrip: no whitespace");
}

// ---------------------------------------------------------------------------
// String search tests
// ---------------------------------------------------------------------------

static void
test_str_search(void)
{
    /* strstr_len */
    test_check(axl_strstr_len("hello world", -1, "world") != NULL,
               "strstr_len: found");
    test_check(axl_strstr_len("hello world", 5, "world") == NULL,
               "strstr_len: bounded miss");
    test_check(axl_strstr_len("hello world", -1, "xyz") == NULL,
               "strstr_len: not found");
    test_check(axl_strstr_len("hello", -1, "") != NULL,
               "strstr_len: empty needle");

    /* strrstr */
    const char *s = "abcabc";
    char *p = axl_strrstr(s, "abc");
    test_check(p == s + 3, "strrstr: finds last");
    test_check(axl_strrstr(s, "xyz") == NULL, "strrstr: not found");

    /* strrstr_len */
    test_check(axl_strrstr_len(s, 4, "abc") == s,
               "strrstr_len: bounded finds first only");
}

// ---------------------------------------------------------------------------
// String testing tests
// ---------------------------------------------------------------------------

static void
test_str_test(void)
{
    /* has_prefix */
    test_check(axl_str_has_prefix("hello world", "hello"),
               "has_prefix: match");
    test_check(!axl_str_has_prefix("hello", "hello world"),
               "has_prefix: prefix longer");
    test_check(axl_str_has_prefix("hello", ""),
               "has_prefix: empty prefix");

    /* has_suffix */
    test_check(axl_str_has_suffix("hello.efi", ".efi"),
               "has_suffix: match");
    test_check(!axl_str_has_suffix(".efi", "hello.efi"),
               "has_suffix: suffix longer");
    test_check(axl_str_has_suffix("hello", ""),
               "has_suffix: empty suffix");

    /* is_ascii */
    test_check(axl_str_is_ascii("hello 123 !@#"),
               "is_ascii: pure ASCII");
    test_check(!axl_str_is_ascii("caf\xc3\xa9"),
               "is_ascii: UTF-8 returns false");
    test_check(axl_str_is_ascii(""), "is_ascii: empty string");

    /* strcmp0 */
    test_check(axl_strcmp0(NULL, NULL) == 0,
               "strcmp0: both NULL equal");
    test_check(axl_strcmp0(NULL, "a") < 0,
               "strcmp0: NULL < non-NULL");
    test_check(axl_strcmp0("a", NULL) > 0,
               "strcmp0: non-NULL > NULL");
    test_check(axl_strcmp0("abc", "abc") == 0,
               "strcmp0: equal strings");

    /* str_equal */
    test_check(axl_str_equal("abc", "abc"), "str_equal: match");
    test_check(!axl_str_equal("abc", "def"), "str_equal: mismatch");

    /* strncasecmp */
    test_check(axl_strncasecmp("Hello", "HELLO", 5) == 0,
               "strncasecmp: equal");
    test_check(axl_strncasecmp("Hello", "Help", 3) == 0,
               "strncasecmp: equal within n");
    test_check(axl_strncasecmp("Hello", "Help", 4) != 0,
               "strncasecmp: differ at n");

    /* strv_contains */
    const char *arr[] = {"alpha", "beta", "gamma", NULL};
    test_check(axl_strv_contains(arr, "beta"),
               "strv_contains: found");
    test_check(!axl_strv_contains(arr, "delta"),
               "strv_contains: not found");

    /* strv_equal */
    const char *arr2[] = {"alpha", "beta", "gamma", NULL};
    const char *arr3[] = {"alpha", "beta", NULL};
    test_check(axl_strv_equal(arr, arr2), "strv_equal: match");
    test_check(!axl_strv_equal(arr, arr3), "strv_equal: different length");
}

// ---------------------------------------------------------------------------
// JSON escape tests
// ---------------------------------------------------------------------------

static void
test_json_escape(void)
{
    char buf[128];
    int  n;

    /* Simple string */
    n = axl_json_escape_string("hello", buf, sizeof(buf));
    test_check(n > 0, "json escape: simple returns > 0");
    test_check(axl_strcmp(buf, "\"hello\"") == 0, "json escape: simple value");

    /* Quotes */
    n = axl_json_escape_string("say \"hi\"", buf, sizeof(buf));
    test_check(n > 0, "json escape: quotes returns > 0");
    test_check(axl_strcmp(buf, "\"say \\\"hi\\\"\"") == 0, "json escape: quotes value");

    /* Backslash */
    n = axl_json_escape_string("a\\b", buf, sizeof(buf));
    test_check(axl_strcmp(buf, "\"a\\\\b\"") == 0, "json escape: backslash");

    /* Newline and tab */
    n = axl_json_escape_string("a\nb\tc", buf, sizeof(buf));
    test_check(axl_strcmp(buf, "\"a\\nb\\tc\"") == 0, "json escape: newline+tab");

    /* Empty string */
    n = axl_json_escape_string("", buf, sizeof(buf));
    test_check(n == 2, "json escape: empty len 2");
    test_check(axl_strcmp(buf, "\"\"") == 0, "json escape: empty value");

    /* Buffer too small */
    test_check(axl_json_escape_string("hello", buf, 4) == -1,
               "json escape: buffer too small");

    /* NULL safety */
    test_check(axl_json_escape_string(NULL, buf, sizeof(buf)) == -1,
               "json escape: NULL src");
    test_check(axl_json_escape_string("hi", NULL, 10) == -1,
               "json escape: NULL out");
}

// ---------------------------------------------------------------------------
// Cache tests
// ---------------------------------------------------------------------------

static void
test_cache(void)
{
    /* Create cache: 4 slots, 4-byte values, 500ms TTL */
    AxlCache *c = axl_cache_new(4, sizeof(int), 500);
    test_check(c != NULL, "cache: new");

    /* Put and get */
    int val = 42;
    test_check(axl_cache_put(c, "key1", &val) == AXL_OK, "cache: put key1");

    int out = 0;
    test_check(axl_cache_get(c, "key1", &out) == AXL_OK, "cache: get key1 hit");
    test_check(out == 42, "cache: get key1 value");

    /* Miss on unknown key */
    test_check(axl_cache_get(c, "missing", &out) == AXL_ERR, "cache: miss");

    /* Overwrite existing */
    val = 99;
    axl_cache_put(c, "key1", &val);
    axl_cache_get(c, "key1", &out);
    test_check(out == 99, "cache: overwrite value");

    /* Invalidate */
    axl_cache_invalidate(c, "key1");
    test_check(axl_cache_get(c, "key1", &out) == AXL_ERR, "cache: invalidated miss");

    /* Fill cache to capacity (4 slots) */
    int v1 = 1, v2 = 2, v3 = 3, v4 = 4, v5 = 5;
    axl_cache_put(c, "a", &v1);
    axl_cache_put(c, "b", &v2);
    axl_cache_put(c, "c", &v3);
    axl_cache_put(c, "d", &v4);

    /* 5th entry evicts oldest (a) */
    axl_cache_put(c, "e", &v5);
    test_check(axl_cache_get(c, "a", &out) == AXL_ERR, "cache: LRU evicted 'a'");
    test_check(axl_cache_get(c, "e", &out) == AXL_OK, "cache: 'e' present");
    test_check(out == 5, "cache: 'e' value");

    /* NULL safety */
    test_check(axl_cache_new(0, 4, 100) == NULL, "cache: new zero slots");
    axl_cache_free(NULL);  /* no crash */
    test_check(true, "cache: free(NULL) no crash");

    axl_cache_free(c);
}

// ---------------------------------------------------------------------------
// Page Cache Tests
// ---------------------------------------------------------------------------

typedef struct {
    uint64_t calls;        // total fill invocations
    size_t   error_page;   // page that returns -1 (SIZE_MAX = none)
    size_t   short_page;   // page that returns a short count (SIZE_MAX = none)
    size_t   short_len;    // count returned for short_page
} PcFill;

// Deterministic synthetic fill: page p, byte i -> (p*31 + i). No file
// needed, so this runs identically on both arches with no fs0: SKIP.
static int64_t
pc_fill(void *user, size_t page_index, void *dst, size_t cap)
{
    PcFill *s = (PcFill *)user;
    s->calls++;
    if (page_index == s->error_page) {
        return -1;
    }
    size_t n = cap;
    if (page_index == s->short_page && s->short_len < cap) {
        n = s->short_len;
    }
    uint8_t *d = (uint8_t *)dst;
    for (size_t i = 0; i < n; i++) {
        d[i] = (uint8_t)(page_index * 31u + i);
    }
    return (int64_t)n;
}

static bool
pc_page_matches(const uint8_t *p, size_t page_index, size_t len)
{
    if (p == NULL) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (p[i] != (uint8_t)(page_index * 31u + i)) {
            return false;
        }
    }
    return true;
}

static void
test_page_cache(void)
{
    PcFill st = { 0, (size_t)-1, (size_t)-1, 0 };
    AxlPageCache *pc = axl_page_cache_new(64, 3, pc_fill, &st);
    test_check(pc != NULL, "page_cache: new");
    test_check(axl_page_cache_page_size(pc) == 64, "page_cache: page size reported");

    size_t vlen = 0;
    const uint8_t *p = (const uint8_t *)axl_page_cache_get(pc, 0, &vlen);
    test_check(p != NULL && vlen == 64, "page_cache: get page 0 full valid_len");
    test_check(pc_page_matches(p, 0, 64), "page_cache: page 0 content");

    p = (const uint8_t *)axl_page_cache_get(pc, 5, &vlen);
    test_check(pc_page_matches(p, 5, 64), "page_cache: page 5 content distinct");

    // Resident re-get: a hit, no extra fill.
    AxlPageCacheStats s;
    axl_page_cache_stats(pc, &s);
    uint64_t fills_before = s.fills;
    (void)axl_page_cache_get(pc, 0, &vlen);
    axl_page_cache_stats(pc, &s);
    test_check(s.fills == fills_before, "page_cache: resident re-get does no fill");
    test_check(s.hits >= 1, "page_cache: hit counted");

    // Fill the 3 frames {5, 0, 10} (recency oldest->newest: 5, 0, 10),
    // then a 4th distinct page must evict the LRU (page 5).
    (void)axl_page_cache_get(pc, 10, &vlen);
    (void)axl_page_cache_get(pc, 20, &vlen);
    axl_page_cache_stats(pc, &s);
    test_check(s.evictions >= 1, "page_cache: eviction at capacity");

    // LRU correctness: 0 and 10 stayed resident; 5 (the LRU) was evicted.
    axl_page_cache_stats(pc, &s);
    uint64_t f0 = s.fills;
    (void)axl_page_cache_get(pc, 0, &vlen);
    (void)axl_page_cache_get(pc, 10, &vlen);
    axl_page_cache_stats(pc, &s);
    test_check(s.fills == f0, "page_cache: LRU kept recently-used pages 0 and 10");
    (void)axl_page_cache_get(pc, 5, &vlen);
    axl_page_cache_stats(pc, &s);
    test_check(s.fills == f0 + 1, "page_cache: LRU evicted the least-recently-used page (5)");

    // Partial trailing page: short fill -> valid_len reflects it.
    st.short_page = 7;
    st.short_len = 20;
    p = (const uint8_t *)axl_page_cache_get(pc, 7, &vlen);
    test_check(p != NULL && vlen == 20, "page_cache: partial page valid_len");
    test_check(pc_page_matches(p, 7, 20), "page_cache: partial page content");

    // Fill error -> NULL.
    st.error_page = 99;
    vlen = 12345;
    p = (const uint8_t *)axl_page_cache_get(pc, 99, &vlen);
    test_check(p == NULL, "page_cache: fill error returns NULL");

    // Clear drops residency + zeroes stats; next get refills.
    axl_page_cache_clear(pc);
    axl_page_cache_stats(pc, &s);
    test_check(s.hits == 0 && s.misses == 0 && s.evictions == 0 && s.fills == 0,
               "page_cache: clear resets stats");
    st.error_page = (size_t)-1;
    st.short_page = (size_t)-1;
    uint64_t calls_before = st.calls;
    (void)axl_page_cache_get(pc, 0, &vlen);
    test_check(st.calls == calls_before + 1, "page_cache: get after clear refills");

    // Arg validation + NULL safety.
    test_check(axl_page_cache_new(0, 3, pc_fill, &st) == NULL, "page_cache: zero page_size -> NULL");
    test_check(axl_page_cache_new(64, 0, pc_fill, &st) == NULL, "page_cache: zero frames -> NULL");
    test_check(axl_page_cache_new(64, 3, NULL, &st) == NULL, "page_cache: NULL fill -> NULL");
    test_check(axl_page_cache_new(SIZE_MAX, 2, pc_fill, &st) == NULL, "page_cache: pool-size overflow -> NULL");
    test_check(axl_page_cache_get(NULL, 0, &vlen) == NULL, "page_cache: get(NULL) -> NULL");
    axl_page_cache_free(NULL);

    axl_page_cache_free(pc);
}

// Owner-distinguishing fill: content = tag(*user) + page*31 + i, so the
// same page index produces different bytes for different owners.
static int64_t
pc_owner_fill(void *user, size_t page_index, void *dst, size_t cap)
{
    uint8_t tag = *(const uint8_t *)user;
    uint8_t *d = (uint8_t *)dst;
    for (size_t i = 0; i < cap; i++) {
        d[i] = (uint8_t)(tag + page_index * 31u + i);
    }
    return (int64_t)cap;
}

static bool
pc_owner_matches(const uint8_t *p, uint8_t tag, size_t page_index, size_t len)
{
    if (p == NULL) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (p[i] != (uint8_t)(tag + page_index * 31u + i)) {
            return false;
        }
    }
    return true;
}

static void
test_page_cache_multitenant(void)
{
    uint8_t owner_a = 0xA0, owner_b = 0xB0;   // identity = &owner_a / &owner_b
    size_t  vlen = 0;
    AxlPageCacheStats s;

    AxlPageCache *pc = axl_page_cache_new_shared(64, 4);
    test_check(pc != NULL, "pc mt: new_shared");
    test_check(axl_page_cache_page_size(pc) == 64, "pc mt: page size");
    test_check(axl_page_cache_get(pc, 0, &vlen) == NULL && vlen == 0,
               "pc mt: single-tenant get() unavailable on shared cache");

    // Same page index, different owners -> distinct frames (no collision).
    const uint8_t *pa = (const uint8_t *)axl_page_cache_fetch(pc, &owner_a, 0,
                                                              pc_owner_fill, &owner_a, &vlen);
    test_check(pa != NULL && vlen == 64 && pc_owner_matches(pa, 0xA0, 0, 64),
               "pc mt: owner A page 0 content");
    const uint8_t *pb = (const uint8_t *)axl_page_cache_fetch(pc, &owner_b, 0,
                                                              pc_owner_fill, &owner_b, &vlen);
    test_check(pb != NULL && pc_owner_matches(pb, 0xB0, 0, 64),
               "pc mt: owner B page 0 distinct from A");

    // Re-fetch A page 0 is a hit (no fill) and still A's content.
    axl_page_cache_stats(pc, &s);
    uint64_t fills0 = s.fills;
    pa = (const uint8_t *)axl_page_cache_fetch(pc, &owner_a, 0, pc_owner_fill, &owner_a, &vlen);
    axl_page_cache_stats(pc, &s);
    test_check(s.fills == fills0 && pc_owner_matches(pa, 0xA0, 0, 64),
               "pc mt: A page 0 resident across tenants (hit)");

    // drop_owner reclaims only A's frames; B's stay resident.
    axl_page_cache_drop_owner(pc, &owner_a);
    axl_page_cache_stats(pc, &s);
    uint64_t fills1 = s.fills;
    (void)axl_page_cache_fetch(pc, &owner_b, 0, pc_owner_fill, &owner_b, &vlen);
    axl_page_cache_stats(pc, &s);
    test_check(s.fills == fills1, "pc mt: drop_owner(A) left B resident");
    (void)axl_page_cache_fetch(pc, &owner_a, 0, pc_owner_fill, &owner_a, &vlen);
    axl_page_cache_stats(pc, &s);
    test_check(s.fills == fills1 + 1, "pc mt: drop_owner(A) evicted A (refill)");

    // NULL fill -> NULL.
    test_check(axl_page_cache_fetch(pc, &owner_a, 0, NULL, NULL, &vlen) == NULL,
               "pc mt: NULL fill -> NULL");
    axl_page_cache_free(pc);

    // Global LRU across tenants: 2 frames, A0 (older) + B0 fill them; a
    // third fetch by owner B evicts the global LRU, which is owner A's page
    // — one tenant's activity reclaims another's frame.
    pc = axl_page_cache_new_shared(64, 2);
    (void)axl_page_cache_fetch(pc, &owner_a, 0, pc_owner_fill, &owner_a, &vlen);  // clock 1
    (void)axl_page_cache_fetch(pc, &owner_b, 0, pc_owner_fill, &owner_b, &vlen);  // clock 2
    axl_page_cache_stats(pc, &s);
    uint64_t fills2 = s.fills;
    (void)axl_page_cache_fetch(pc, &owner_b, 1, pc_owner_fill, &owner_b, &vlen);  // evicts A0 (LRU)
    axl_page_cache_stats(pc, &s);
    test_check(s.evictions >= 1 && s.fills == fills2 + 1, "pc mt: global LRU eviction across tenants");
    uint64_t fills3 = s.fills;
    (void)axl_page_cache_fetch(pc, &owner_a, 0, pc_owner_fill, &owner_a, &vlen);  // A0 was evicted by B
    axl_page_cache_stats(pc, &s);
    test_check(s.fills == fills3 + 1, "pc mt: a tenant's fetch evicted another tenant's page");
    axl_page_cache_free(pc);
}

// ---------------------------------------------------------------------------
// RB Tree Tests (generic intrusive augmented red-black tree)
// ---------------------------------------------------------------------------

typedef struct {
    AxlRBNode node;
    int       key;
    long      val;
    size_t    sub_count;   // augment: # nodes in subtree
    long      sub_sum;     // augment: sum of val over subtree
} RbEnt;

#define RB_ENT(n) AXL_RB_ENTRY(n, RbEnt, node)

static void
rb_recompute(AxlRBNode *n, void *user)
{
    (void)user;
    RbEnt *e = RB_ENT(n);
    size_t c = 1;
    long   s = e->val;
    if (n->left != NULL) {
        c += RB_ENT(n->left)->sub_count;
        s += RB_ENT(n->left)->sub_sum;
    }
    if (n->right != NULL) {
        c += RB_ENT(n->right)->sub_count;
        s += RB_ENT(n->right)->sub_sum;
    }
    e->sub_count = c;
    e->sub_sum = s;
}

/* Recursive RB-invariant checker: parent links, no red-red, equal black
   height on all paths. Returns black height, sets *ok=false on any
   violation. NULL is a black sentinel (height 1). */
static int
rb_check(const AxlRBNode *n, const AxlRBNode *parent, bool *ok)
{
    if (n == NULL) {
        return 1;
    }
    if (n->parent != parent) {
        *ok = false;
    }
    if (n->color == AXL_RB_RED) {
        if ((n->left != NULL && n->left->color == AXL_RB_RED) ||
            (n->right != NULL && n->right->color == AXL_RB_RED)) {
            *ok = false;
        }
    }
    int lh = rb_check(n->left, n, ok);
    int rh = rb_check(n->right, n, ok);
    if (lh != rh) {
        *ok = false;
    }
    return lh + (n->color == AXL_RB_BLACK ? 1 : 0);
}

/* Brute-force augment check: cached aggregates must equal a fresh
   bottom-up recomputation. */
static void
rb_aug_check(const AxlRBNode *n, bool *ok)
{
    if (n == NULL) {
        return;
    }
    rb_aug_check(n->left, ok);
    rb_aug_check(n->right, ok);
    size_t c = 1;
    long   s = RB_ENT(n)->val;
    if (n->left != NULL) {
        c += RB_ENT(n->left)->sub_count;
        s += RB_ENT(n->left)->sub_sum;
    }
    if (n->right != NULL) {
        c += RB_ENT(n->right)->sub_count;
        s += RB_ENT(n->right)->sub_sum;
    }
    if (RB_ENT(n)->sub_count != c || RB_ENT(n)->sub_sum != s) {
        *ok = false;
    }
}

static bool
rb_insert_key(AxlRBTree *t, RbEnt *e)
{
    AxlRBNode **link = &t->root;
    AxlRBNode  *parent = NULL;
    while (*link != NULL) {
        parent = *link;
        RbEnt *p = RB_ENT(parent);
        if (e->key < p->key) {
            link = &parent->left;
        } else if (e->key > p->key) {
            link = &parent->right;
        } else {
            return false;   // duplicate key
        }
    }
    axl_rb_link_node(&e->node, parent, link);
    axl_rb_insert(t, &e->node);
    return true;
}

/* k-th smallest (0-based) via the sub_count augment. */
static AxlRBNode *
rb_select(AxlRBTree *t, size_t k)
{
    AxlRBNode *n = t->root;
    while (n != NULL) {
        size_t lc = (n->left != NULL) ? RB_ENT(n->left)->sub_count : 0;
        if (k < lc) {
            n = n->left;
        } else if (k == lc) {
            return n;
        } else {
            k -= lc + 1;
            n = n->right;
        }
    }
    return NULL;
}

static RbEnt rb_pool[300];
static RbEnt rb_pool2[10];

static void
test_rb_tree(void)
{
    AxlRBTree t;
    axl_rb_tree_init(&t, rb_recompute, NULL);
    test_check(axl_rb_tree_empty(&t), "rbtree: empty after init");
    test_check(axl_rb_first(&t) == NULL && axl_rb_last(&t) == NULL,
               "rbtree: first/last NULL when empty");

    // Insert 300 distinct keys via a deterministic LCG.
    uint32_t lcg = 12345u;
    size_t n = 0;
    for (int attempts = 0; n < 300 && attempts < 100000; attempts++) {
        lcg = lcg * 1103515245u + 12345u;
        int key = (int)((lcg >> 8) % 100000u);
        RbEnt *e = &rb_pool[n];
        e->key = key;
        e->val = (long)key * 2 + 1;
        if (rb_insert_key(&t, e)) {
            n++;
        }
    }
    test_check(n == 300, "rbtree: inserted 300 distinct keys");

    bool ok = true;
    rb_check(t.root, NULL, &ok);
    test_check(ok && t.root != NULL && t.root->color == AXL_RB_BLACK,
               "rbtree: RB invariants hold after inserts");
    ok = true;
    rb_aug_check(t.root, &ok);
    test_check(ok, "rbtree: augment correct after inserts");

    // In-order ascending, visits all; collect for select cross-check.
    static int inorder[300];
    size_t cnt = 0;
    int prev = -1;
    bool sorted = true;
    long total = 0;
    for (AxlRBNode *it = axl_rb_first(&t); it != NULL; it = axl_rb_next(it)) {
        RbEnt *e = RB_ENT(it);
        if (e->key <= prev) {
            sorted = false;
        }
        prev = e->key;
        if (cnt < 300) {
            inorder[cnt] = e->key;
        }
        total += e->val;
        cnt++;
    }
    test_check(sorted, "rbtree: in-order traversal is ascending");
    test_check(cnt == 300, "rbtree: in-order visits all 300");
    test_check(RB_ENT(t.root)->sub_count == 300, "rbtree: root subtree count == n");
    test_check(RB_ENT(t.root)->sub_sum == total, "rbtree: root subtree sum correct");

    // axl_rb_last == in-order maximum.
    test_check(RB_ENT(axl_rb_last(&t))->key == inorder[299], "rbtree: last == max key");

    // select(k) matches in-order k-th.
    bool sel_ok = true;
    for (size_t k = 0; k < 300; k += 37) {
        AxlRBNode *s = rb_select(&t, k);
        if (s == NULL || RB_ENT(s)->key != inorder[k]) {
            sel_ok = false;
        }
    }
    test_check(sel_ok, "rbtree: select(k) matches in-order k-th (order statistic)");

    // Erase every other inserted node; re-check invariants + augment.
    for (size_t i = 0; i < 300; i += 2) {
        axl_rb_erase(&t, &rb_pool[i].node);
    }
    ok = true;
    rb_check(t.root, NULL, &ok);
    test_check(ok && (t.root == NULL || t.root->color == AXL_RB_BLACK),
               "rbtree: RB invariants hold after erase batch");
    ok = true;
    rb_aug_check(t.root, &ok);
    test_check(ok, "rbtree: augment correct after erase batch");
    size_t rem = 0;
    for (AxlRBNode *it = axl_rb_first(&t); it != NULL; it = axl_rb_next(it)) {
        rem++;
    }
    test_check(rem == 150, "rbtree: 150 remain after erasing 150");

    // update_augment after an in-place payload change.
    RbEnt *u = &rb_pool[1];   // odd index -> still present
    long root_sum_before = RB_ENT(t.root)->sub_sum;
    u->val += 1000;
    axl_rb_update_augment(&t, &u->node);
    ok = true;
    rb_aug_check(t.root, &ok);
    test_check(ok, "rbtree: augment correct after update_augment");
    test_check(RB_ENT(t.root)->sub_sum == root_sum_before + 1000,
               "rbtree: update_augment propagated to root");

    // Erase all remaining down to empty.
    AxlRBNode *it;
    while ((it = axl_rb_first(&t)) != NULL) {
        axl_rb_erase(&t, it);
    }
    test_check(axl_rb_tree_empty(&t), "rbtree: empty after erasing all");

    // NULL recompute = plain balanced tree (no augmentation work).
    AxlRBTree t2;
    axl_rb_tree_init(&t2, NULL, NULL);
    for (int i = 0; i < 10; i++) {
        rb_pool2[i].key = (i * 7) % 11;   // distinct in 0..10
        rb_pool2[i].val = i;
        rb_insert_key(&t2, &rb_pool2[i]);
    }
    ok = true;
    rb_check(t2.root, NULL, &ok);
    test_check(ok, "rbtree: NULL recompute still balances correctly");
    axl_rb_erase(&t2, &rb_pool2[3].node);
    axl_rb_erase(&t2, &rb_pool2[7].node);
    ok = true;
    rb_check(t2.root, NULL, &ok);
    size_t c2 = 0;
    for (AxlRBNode *i2 = axl_rb_first(&t2); i2 != NULL; i2 = axl_rb_next(i2)) {
        c2++;
    }
    test_check(ok && c2 == 8, "rbtree: NULL recompute erase keeps balance + count");
}

// ---------------------------------------------------------------------------
// Text Buffer Tests
// ---------------------------------------------------------------------------

static bool
tb_content_is(const AxlTextBuffer *tb, const char *expect)
{
    size_t elen = axl_strlen(expect);
    if (axl_text_buffer_length(tb) != elen) {
        return false;
    }
    char buf[256];
    if (elen >= sizeof(buf)) {
        return false;
    }
    size_t got = axl_text_buffer_get(tb, 0, elen, buf, sizeof(buf));
    return got == elen && axl_memcmp(buf, expect, elen) == 0;
}

static void
test_text_buffer(void)
{
    size_t st = 0, en = 0;

    AxlTextBuffer *tb = axl_text_buffer_new(0);
    test_check(tb != NULL, "text_buffer: new");

    // Empty buffer is one line.
    test_check(axl_text_buffer_length(tb) == 0, "text_buffer: empty length 0");
    test_check(axl_text_buffer_line_count(tb) == 1, "text_buffer: empty line_count 1");
    test_check(axl_text_buffer_line_of_offset(tb, 0) == 0, "text_buffer: empty line_of_offset 0");
    test_check(axl_text_buffer_byte_at(tb, 0) == -1, "text_buffer: empty byte_at -1");
    test_check(axl_text_buffer_line_bounds(tb, 0, &st, &en) == AXL_OK && st == 0 && en == 0,
               "text_buffer: empty line 0 bounds [0,0)");
    test_check(axl_text_buffer_line_bounds(tb, 1, &st, &en) == AXL_ERR,
               "text_buffer: invalid line -> ERR");

    // Single line.
    test_check(axl_text_buffer_set_bytes(tb, "hello", 5) == AXL_OK, "text_buffer: set_bytes");
    test_check(tb_content_is(tb, "hello"), "text_buffer: content hello");
    test_check(axl_text_buffer_length(tb) == 5, "text_buffer: length 5");
    test_check(axl_text_buffer_line_count(tb) == 1, "text_buffer: single line_count 1");
    test_check(axl_text_buffer_byte_at(tb, 0) == 'h' && axl_text_buffer_byte_at(tb, 4) == 'o',
               "text_buffer: byte_at");
    test_check(axl_text_buffer_byte_at(tb, 5) == -1, "text_buffer: byte_at end -1");

    // Multi-line.
    test_check(axl_text_buffer_set_bytes(tb, "ab\ncd\nef", 8) == AXL_OK, "text_buffer: set multi-line");
    test_check(axl_text_buffer_line_count(tb) == 3, "text_buffer: 3 lines");
    test_check(axl_text_buffer_line_bounds(tb, 0, &st, &en) == AXL_OK && st == 0 && en == 2,
               "text_buffer: line0 [0,2)");
    test_check(axl_text_buffer_line_bounds(tb, 1, &st, &en) == AXL_OK && st == 3 && en == 5,
               "text_buffer: line1 [3,5)");
    test_check(axl_text_buffer_line_bounds(tb, 2, &st, &en) == AXL_OK && st == 6 && en == 8,
               "text_buffer: line2 [6,8)");
    test_check(axl_text_buffer_line_of_offset(tb, 0) == 0, "text_buffer: off0 -> line0");
    test_check(axl_text_buffer_line_of_offset(tb, 2) == 0, "text_buffer: off2 (the '\\n') -> line0");
    test_check(axl_text_buffer_line_of_offset(tb, 3) == 1, "text_buffer: off3 -> line1");
    test_check(axl_text_buffer_line_of_offset(tb, 8) == 2, "text_buffer: off end -> line2");

    // Trailing newline -> real empty last line.
    test_check(axl_text_buffer_set_bytes(tb, "abc\n", 4) == AXL_OK, "text_buffer: set trailing nl");
    test_check(axl_text_buffer_line_count(tb) == 2, "text_buffer: trailing nl -> 2 lines");
    test_check(axl_text_buffer_line_bounds(tb, 1, &st, &en) == AXL_OK && st == 4 && en == 4,
               "text_buffer: empty last line [4,4)");

    // Consecutive newlines -> empty middle line.
    test_check(axl_text_buffer_set_bytes(tb, "a\n\nb", 4) == AXL_OK, "text_buffer: set consecutive nl");
    test_check(axl_text_buffer_line_count(tb) == 3, "text_buffer: consecutive nl -> 3 lines");
    test_check(axl_text_buffer_line_bounds(tb, 1, &st, &en) == AXL_OK && st == 2 && en == 2,
               "text_buffer: empty middle line [2,2)");

    // Insert (no newline) + read across the gap.
    test_check(axl_text_buffer_set_bytes(tb, "helloworld", 10) == AXL_OK, "text_buffer: reset");
    test_check(axl_text_buffer_insert(tb, 5, " ", 1) == AXL_OK, "text_buffer: insert space");
    test_check(tb_content_is(tb, "hello world"), "text_buffer: insert content");
    test_check(axl_text_buffer_length(tb) == 11, "text_buffer: insert length 11");
    char g[16];
    size_t gn = axl_text_buffer_get(tb, 3, 5, g, sizeof(g));   // "lo wo" straddles the gap
    test_check(gn == 5 && axl_memcmp(g, "lo wo", 5) == 0, "text_buffer: get across gap");
    test_check(axl_text_buffer_byte_at(tb, 6) == 'w', "text_buffer: byte_at across gap");

    // Insert with newlines updates the index incrementally.
    test_check(axl_text_buffer_insert(tb, 5, "\nX\n", 3) == AXL_OK, "text_buffer: insert newlines");
    test_check(tb_content_is(tb, "hello\nX\n world"), "text_buffer: insert-newlines content");
    test_check(axl_text_buffer_line_count(tb) == 3, "text_buffer: insert-newlines line_count 3");
    test_check(axl_text_buffer_line_bounds(tb, 0, &st, &en) == AXL_OK && st == 0 && en == 5,
               "text_buffer: il line0 [0,5)");
    test_check(axl_text_buffer_line_bounds(tb, 1, &st, &en) == AXL_OK && st == 6 && en == 7,
               "text_buffer: il line1 [6,7)");
    test_check(axl_text_buffer_line_bounds(tb, 2, &st, &en) == AXL_OK && st == 8 && en == 14,
               "text_buffer: il line2 [8,14)");

    // Prepend, append, and offset clamping.
    test_check(axl_text_buffer_set_bytes(tb, "mid", 3) == AXL_OK, "text_buffer: reset mid");
    test_check(axl_text_buffer_insert(tb, 0, "pre-", 4) == AXL_OK, "text_buffer: prepend");
    test_check(axl_text_buffer_insert(tb, axl_text_buffer_length(tb), "-post", 5) == AXL_OK,
               "text_buffer: append");
    test_check(axl_text_buffer_insert(tb, 999, "!", 1) == AXL_OK, "text_buffer: insert clamps offset");
    test_check(tb_content_is(tb, "pre-mid-post!"), "text_buffer: prepend/append/clamp content");

    // Delete a range.
    test_check(axl_text_buffer_set_bytes(tb, "hello world", 11) == AXL_OK, "text_buffer: reset for delete");
    test_check(axl_text_buffer_delete(tb, 5, 6) == AXL_OK, "text_buffer: delete tail");
    test_check(tb_content_is(tb, "hello"), "text_buffer: delete content");

    // Delete spanning a newline collapses the two lines.
    test_check(axl_text_buffer_set_bytes(tb, "ab\ncd", 5) == AXL_OK, "text_buffer: reset 2-line");
    test_check(axl_text_buffer_line_count(tb) == 2, "text_buffer: pre-delete 2 lines");
    test_check(axl_text_buffer_delete(tb, 1, 3) == AXL_OK, "text_buffer: delete across nl");
    test_check(tb_content_is(tb, "ad"), "text_buffer: delete-across-nl content");
    test_check(axl_text_buffer_line_count(tb) == 1, "text_buffer: delete merged to 1 line");

    // Delete length clamping + offset past end is a no-op.
    test_check(axl_text_buffer_set_bytes(tb, "abc", 3) == AXL_OK, "text_buffer: reset abc");
    test_check(axl_text_buffer_delete(tb, 1, 999) == AXL_OK && tb_content_is(tb, "a"),
               "text_buffer: delete len clamps");
    test_check(axl_text_buffer_delete(tb, 999, 5) == AXL_OK && tb_content_is(tb, "a"),
               "text_buffer: delete past end no-op");

    // get truncation at cap and clamping at end.
    test_check(axl_text_buffer_set_bytes(tb, "abcdef", 6) == AXL_OK, "text_buffer: reset abcdef");
    char tr[4];
    size_t tn = axl_text_buffer_get(tb, 0, 6, tr, 3);
    test_check(tn == 3 && axl_memcmp(tr, "abc", 3) == 0, "text_buffer: get truncates at cap");
    tn = axl_text_buffer_get(tb, 4, 100, tr, sizeof(tr));
    test_check(tn == 2 && axl_memcmp(tr, "ef", 2) == 0, "text_buffer: get clamps at end");
    test_check(axl_text_buffer_get(tb, 6, 5, tr, sizeof(tr)) == 0, "text_buffer: get at end -> 0");

    // Many incremental edits keep the line index correct.
    test_check(axl_text_buffer_set_bytes(tb, "", 0) == AXL_OK, "text_buffer: clear");
    bool many_ok = true;
    for (int i = 0; i < 100; i++) {
        if (axl_text_buffer_insert(tb, axl_text_buffer_length(tb), "line\n", 5) != AXL_OK) {
            many_ok = false;
        }
    }
    test_check(many_ok, "text_buffer: 100 appends ok");
    test_check(axl_text_buffer_line_count(tb) == 101, "text_buffer: 100 lines + empty tail = 101");
    test_check(axl_text_buffer_line_bounds(tb, 50, &st, &en) == AXL_OK && st == 50u * 5u && en == 50u * 5u + 4u,
               "text_buffer: line 50 bounds after many edits");
    test_check(axl_text_buffer_line_of_offset(tb, 50u * 5u + 2u) == 50,
               "text_buffer: line_of_offset after many edits");

    axl_text_buffer_free(tb);

    // NULL safety.
    axl_text_buffer_free(NULL);
    test_check(axl_text_buffer_length(NULL) == 0, "text_buffer: length(NULL) 0");
    test_check(axl_text_buffer_line_count(NULL) == 1, "text_buffer: line_count(NULL) 1");
    test_check(axl_text_buffer_byte_at(NULL, 0) == -1, "text_buffer: byte_at(NULL) -1");

    // OOM: new() and a grow that must leave the buffer intact.
    axl_mem_fail_next_alloc(1);
    test_check(axl_text_buffer_new(64) == NULL, "text_buffer: OOM on new -> NULL");

    AxlTextBuffer *small = axl_text_buffer_new(2);
    test_check(small != NULL, "text_buffer: new small");
    axl_mem_fail_next_alloc(1);
    test_check(axl_text_buffer_insert(small, 0, "abcdefghij", 10) == AXL_ERR,
               "text_buffer: insert grow OOM -> ERR");
    test_check(axl_text_buffer_length(small) == 0, "text_buffer: buffer intact after grow OOM");
    axl_text_buffer_free(small);
}

// get_alloc + UTF-8 codepoint nav parity with AxlPieceTree.
static void
test_text_buffer_nav(void)
{
    AxlTextBuffer *tb = axl_text_buffer_new(0);
    (void)axl_text_buffer_set_bytes(tb, "hello world", 11);

    char *s = axl_text_buffer_get_alloc(tb, 0, 5);
    test_check(s != NULL && axl_strcmp(s, "hello") == 0, "tb get_alloc: exact range");
    axl_free(s);
    s = axl_text_buffer_get_alloc(tb, 6, 999);
    test_check(s != NULL && axl_strcmp(s, "world") == 0, "tb get_alloc: clamped to length");
    axl_free(s);
    s = axl_text_buffer_get_alloc(tb, 50, 5);
    test_check(s != NULL && s[0] == '\0', "tb get_alloc: past end -> empty");
    axl_free(s);
    test_check(axl_text_buffer_get_alloc(NULL, 0, 1) == NULL, "tb get_alloc: NULL -> NULL");

    /* a(1) é(2) €(3) 𐍈(4) b(1): boundaries 0,1,3,6,10,11 */
    const unsigned char mb[] = { 'a', 0xC3,0xA9, 0xE2,0x82,0xAC, 0xF0,0x90,0x8D,0x88, 'b' };
    (void)axl_text_buffer_set_bytes(tb, (const char *)mb, sizeof(mb));
    size_t b[] = { 0, 1, 3, 6, 10, 11 };
    bool fwd = true, bwd = true;
    for (size_t i = 0; i + 1 < sizeof(b) / sizeof(b[0]); i++) {
        if (axl_text_buffer_cp_next(tb, b[i]) != b[i + 1]) {
            fwd = false;
        }
        if (axl_text_buffer_cp_prev(tb, b[i + 1]) != b[i]) {
            bwd = false;
        }
    }
    test_check(fwd && axl_text_buffer_cp_next(tb, 11) == 11, "tb cp_next: walks boundaries");
    test_check(bwd && axl_text_buffer_cp_prev(tb, 0) == 0, "tb cp_prev: walks boundaries");
    test_check(axl_text_buffer_cp_align(tb, 4) == 3 && axl_text_buffer_cp_align(tb, 5) == 3
               && axl_text_buffer_cp_align(tb, 6) == 6,
               "tb cp_align: snaps mid-codepoint down");
    axl_text_buffer_free(tb);
}

// ---------------------------------------------------------------------------
// Radix Tree Tests
// ---------------------------------------------------------------------------

static void
test_radix_tree(void)
{
    AxlRadixTree *t = axl_radix_tree_new();
    test_check(t != NULL, "radix: new");
    if (t == NULL) {
        return;
    }

    test_check(axl_radix_tree_size(t) == 0, "radix: initial size 0");

    /* Insert 3 keys */
    size_t v1 = 100, v2 = 200, v3 = 300;
    test_check(axl_radix_tree_insert(t, "/api", &v1) == AXL_OK, "radix: insert /api");
    test_check(axl_radix_tree_insert(t, "/api/users", &v2) == AXL_OK, "radix: insert /api/users");
    test_check(axl_radix_tree_insert(t, "/index.html", &v3) == AXL_OK, "radix: insert /index.html");
    test_check(axl_radix_tree_size(t) == 3, "radix: size 3");

    /* Exact lookup */
    test_check(axl_radix_tree_lookup(t, "/api") == &v1, "radix: lookup /api");
    test_check(axl_radix_tree_lookup(t, "/api/users") == &v2, "radix: lookup /api/users");
    test_check(axl_radix_tree_lookup(t, "/index.html") == &v3, "radix: lookup /index.html");
    test_check(axl_radix_tree_lookup(t, "/missing") == NULL, "radix: lookup missing");
    test_check(axl_radix_tree_lookup(t, "/api/us") == NULL, "radix: lookup partial");

    /* Remove */
    test_check(axl_radix_tree_remove(t, "/api") == true, "radix: remove /api");
    test_check(axl_radix_tree_size(t) == 2, "radix: size 2 after remove");
    test_check(axl_radix_tree_lookup(t, "/api") == NULL, "radix: lookup removed");
    test_check(axl_radix_tree_lookup(t, "/api/users") == &v2, "radix: sibling intact");

    /* Remove non-existent */
    test_check(axl_radix_tree_remove(t, "/nope") == false, "radix: remove missing");

    /* Overwrite */
    size_t v4 = 400;
    test_check(axl_radix_tree_insert(t, "/api/users", &v4) == AXL_OK, "radix: overwrite");
    test_check(axl_radix_tree_lookup(t, "/api/users") == &v4, "radix: overwrite value");
    test_check(axl_radix_tree_size(t) == 2, "radix: size unchanged after overwrite");

    /* Empty key */
    size_t v5 = 500;
    test_check(axl_radix_tree_insert(t, "", &v5) == AXL_OK, "radix: insert empty key");
    test_check(axl_radix_tree_lookup(t, "") == &v5, "radix: lookup empty key");
    test_check(axl_radix_tree_size(t) == 3, "radix: size 3 with empty key");

    /* NULL safety */
    test_check(axl_radix_tree_lookup(NULL, "x") == NULL, "radix: lookup NULL tree");
    test_check(axl_radix_tree_insert(NULL, "x", NULL) == AXL_ERR, "radix: insert NULL tree");
    axl_radix_tree_free(NULL);
    test_check(true, "radix: free(NULL) no crash");

    axl_radix_tree_free(t);
}

static void
test_radix_tree_prefix(void)
{
    AxlRadixTree *t = axl_radix_tree_new();
    if (t == NULL) {
        return;
    }

    size_t v1 = 1, v2 = 2, v3 = 3;
    axl_radix_tree_insert(t, "/api/users", &v1);
    axl_radix_tree_insert(t, "/api", &v2);
    axl_radix_tree_insert(t, "/css/", &v3);

    const char *suffix = NULL;
    void *val;

    /* /api/users/42 → longest prefix is /api/users */
    val = axl_radix_tree_lookup_prefix(t, "/api/users/42", &suffix);
    test_check(val == &v1, "radix_prefix: /api/users/42 → /api/users");
    test_check(suffix != NULL && axl_strcmp(suffix, "/42") == 0,
               "radix_prefix: suffix is /42");

    /* /api/health → longest prefix is /api */
    val = axl_radix_tree_lookup_prefix(t, "/api/health", &suffix);
    test_check(val == &v2, "radix_prefix: /api/health → /api");
    test_check(suffix != NULL && axl_strcmp(suffix, "/health") == 0,
               "radix_prefix: suffix is /health");

    /* /css/style.css → longest prefix is /css/ */
    val = axl_radix_tree_lookup_prefix(t, "/css/style.css", &suffix);
    test_check(val == &v3, "radix_prefix: /css/style.css → /css/");
    test_check(suffix != NULL && axl_strcmp(suffix, "style.css") == 0,
               "radix_prefix: suffix is style.css");

    /* /other → no prefix match, suffix unchanged */
    suffix = (const char *)0xDEAD;
    val = axl_radix_tree_lookup_prefix(t, "/other", &suffix);
    test_check(val == NULL, "radix_prefix: /other → no match");
    test_check(suffix == (const char *)0xDEAD,
               "radix_prefix: suffix unchanged on no match");

    /* NULL suffix pointer is accepted */
    val = axl_radix_tree_lookup_prefix(t, "/api/users/42", NULL);
    test_check(val == &v1, "radix_prefix: NULL suffix OK");

    /* Exact match works as prefix too */
    val = axl_radix_tree_lookup_prefix(t, "/api/users", &suffix);
    test_check(val == &v1, "radix_prefix: exact match /api/users");
    test_check(suffix != NULL && suffix[0] == '\0',
               "radix_prefix: exact match empty suffix");

    axl_radix_tree_free(t);
}

static void
test_radix_tree_edge_split(void)
{
    AxlRadixTree *t = axl_radix_tree_new();
    if (t == NULL) {
        return;
    }

    size_t v1 = 1, v2 = 2, v3 = 3;

    /* Insert "test" first, then "team" forces split at "te" */
    axl_radix_tree_insert(t, "test", &v1);
    axl_radix_tree_insert(t, "team", &v2);
    axl_radix_tree_insert(t, "tea", &v3);

    test_check(axl_radix_tree_lookup(t, "test") == &v1, "radix_split: lookup test");
    test_check(axl_radix_tree_lookup(t, "team") == &v2, "radix_split: lookup team");
    test_check(axl_radix_tree_lookup(t, "tea") == &v3, "radix_split: lookup tea");
    test_check(axl_radix_tree_lookup(t, "te") == NULL, "radix_split: te is intermediate");
    test_check(axl_radix_tree_size(t) == 3, "radix_split: size 3");

    /* Remove "tea" — should collapse "te"+"am" → "team" */
    axl_radix_tree_remove(t, "tea");
    test_check(axl_radix_tree_lookup(t, "test") == &v1, "radix_split: test after remove tea");
    test_check(axl_radix_tree_lookup(t, "team") == &v2, "radix_split: team after remove tea");
    test_check(axl_radix_tree_size(t) == 2, "radix_split: size 2 after remove");

    /* Many keys to stress edge splitting */
    size_t vals[10];
    const char *keys[] = {
        "app", "apple", "application", "apply", "apt",
        "ape", "apex", "api", "apricot", "april"
    };

    for (size_t i = 0; i < 10; i++) {
        vals[i] = i + 100;
        axl_radix_tree_insert(t, keys[i], &vals[i]);
    }

    test_check(axl_radix_tree_size(t) == 12, "radix_split: size 12 after bulk");

    for (size_t i = 0; i < 10; i++) {
        test_check(axl_radix_tree_lookup(t, keys[i]) == &vals[i],
                   "radix_split: bulk lookup");
    }

    axl_radix_tree_free(t);
}

static size_t radix_foreach_count;

static void
radix_foreach_counter(const void *key, void *value, void *data)
{
    (void)key;
    (void)value;
    (void)data;
    radix_foreach_count++;
}

static void
test_radix_tree_foreach(void)
{
    AxlRadixTree *t = axl_radix_tree_new();
    if (t == NULL) {
        return;
    }

    size_t v = 1;
    axl_radix_tree_insert(t, "/a", &v);
    axl_radix_tree_insert(t, "/b", &v);
    axl_radix_tree_insert(t, "/c", &v);
    axl_radix_tree_insert(t, "/a/x", &v);
    axl_radix_tree_insert(t, "/a/y", &v);

    radix_foreach_count = 0;
    axl_radix_tree_foreach(t, radix_foreach_counter, NULL);
    test_check(radix_foreach_count == 5, "radix_foreach: count 5");

    axl_radix_tree_free(t);
}

static size_t radix_free_count;

static void
radix_free_counter(void *data)
{
    (void)data;
    radix_free_count++;
}

static void
test_radix_tree_value_free(void)
{
    AxlRadixTree *t = axl_radix_tree_new_full(radix_free_counter);
    if (t == NULL) {
        return;
    }

    radix_free_count = 0;
    axl_radix_tree_insert(t, "a", (void *)1);
    axl_radix_tree_insert(t, "b", (void *)2);
    axl_radix_tree_insert(t, "c", (void *)3);

    /* Overwrite calls value_free on old value */
    axl_radix_tree_insert(t, "a", (void *)10);
    test_check(radix_free_count == 1, "radix_vfree: overwrite calls free");

    /* Remove calls value_free */
    radix_free_count = 0;
    axl_radix_tree_remove(t, "b");
    test_check(radix_free_count == 1, "radix_vfree: remove calls free");

    /* Free tree calls value_free on remaining 2 entries */
    radix_free_count = 0;
    axl_radix_tree_free(t);
    test_check(radix_free_count == 2, "radix_vfree: free calls free on remaining");
}

static void
test_radix_tree_http_keys(void)
{
    /* Test with HTTP-style "METHOD /path" keys like the server will use */
    AxlRadixTree *t = axl_radix_tree_new();
    if (t == NULL) {
        return;
    }

    size_t v1 = 1, v2 = 2, v3 = 3, v4 = 4;
    axl_radix_tree_insert(t, "GET /api/users", &v1);
    axl_radix_tree_insert(t, "POST /api/users", &v2);
    axl_radix_tree_insert(t, "GET /css/", &v3);
    axl_radix_tree_insert(t, "* /health", &v4);

    /* Exact lookups */
    test_check(axl_radix_tree_lookup(t, "GET /api/users") == &v1,
               "radix_http: GET /api/users");
    test_check(axl_radix_tree_lookup(t, "POST /api/users") == &v2,
               "radix_http: POST /api/users");
    test_check(axl_radix_tree_lookup(t, "* /health") == &v4,
               "radix_http: * /health");

    /* Prefix lookup for static files */
    const char *suffix = NULL;
    void *val = axl_radix_tree_lookup_prefix(t, "GET /css/style.css", &suffix);
    test_check(val == &v3, "radix_http: prefix /css/style.css");
    test_check(suffix != NULL && axl_strcmp(suffix, "style.css") == 0,
               "radix_http: prefix suffix");

    /* No cross-method matching */
    test_check(axl_radix_tree_lookup(t, "DELETE /api/users") == NULL,
               "radix_http: no cross-method");

    axl_radix_tree_free(t);
}

// ---------------------------------------------------------------------------
// AxlNTree (n-ary tree) Tests
// ---------------------------------------------------------------------------

/* Concatenate a node's immediate children's (string) data into @out. */
static void
ntree_children_str(AxlNTree *parent, char *out)
{
    size_t k = 0;
    for (AxlNTree *c = parent->children; c != NULL; c = c->next) {
        for (const char *s = c->data; *s != '\0'; s++) {
            out[k++] = *s;
        }
    }
    out[k] = '\0';
}

typedef struct { char buf[64]; } NtSeq;

/* Traverse collector: append node data to the running sequence. */
static bool
ntree_collect(AxlNTree *node, void *user)
{
    NtSeq *q = user;
    size_t k = 0;
    while (q->buf[k] != '\0') {
        k++;
    }
    for (const char *s = node->data; *s != '\0'; s++) {
        q->buf[k++] = *s;
    }
    q->buf[k] = '\0';
    return false;   /* continue */
}

/* Collector that stops after the first node. */
static bool
ntree_collect_stop(AxlNTree *node, void *user)
{
    ntree_collect(node, user);
    return true;   /* stop */
}

static void
ntree_foreach_collect(AxlNTree *node, void *user)
{
    (void)ntree_collect(node, user);
}

static int ntree_free_calls;
static void
ntree_count_free(void *data)
{
    (void)data;
    ntree_free_calls++;
}

/* Build the canonical test tree:
 *   R -> [A -> [A1, A2], B, C -> [C1]]   (returns the root). */
static AxlNTree *
ntree_build_sample(void)
{
    AxlNTree *root = axl_ntree_new("R");
    AxlNTree *a = axl_ntree_append_data(root, "A");
    (void)axl_ntree_append_data(root, "B");
    AxlNTree *c = axl_ntree_append_data(root, "C");
    (void)axl_ntree_append_data(a, "A1");
    (void)axl_ntree_append_data(a, "A2");
    (void)axl_ntree_append_data(c, "C1");
    return root;
}

static void
test_ntree_build_navigate(void)
{
    AxlNTree *root = ntree_build_sample();
    AxlNTree *a = axl_ntree_first_child(root);
    AxlNTree *c = axl_ntree_last_child(root);

    test_check(axl_ntree_n_children(root) == 3, "ntree: root has 3 children");
    test_check(axl_ntree_n_children(a) == 2, "ntree: 'A' has 2 children");
    test_check(axl_ntree_n_children(axl_ntree_nth_child(root, 1)) == 0,
               "ntree: 'B' is a leaf");
    test_check(axl_strcmp(a->data, "A") == 0, "ntree: first_child is 'A'");
    test_check(axl_strcmp(c->data, "C") == 0, "ntree: last_child is 'C'");
    test_check(axl_strcmp(axl_ntree_nth_child(root, 1)->data, "B") == 0,
               "ntree: nth_child(1) is 'B'");
    test_check(axl_ntree_nth_child(root, 3) == NULL,
               "ntree: nth_child past end is NULL");
    test_check(a->parent == root && a->next != NULL &&
               axl_strcmp(a->next->data, "B") == 0,
               "ntree: parent + next-sibling links wired");
    test_check(a->next->prev == a && c->parent == root && c->next == NULL,
               "ntree: prev/next/parent are consistent");

    /* Insert ordering in a fresh parent: prepend / append / before / after. */
    AxlNTree *p  = axl_ntree_new("P");
    AxlNTree *n1 = axl_ntree_append_data(p, "1");      /* [1]       */
    AxlNTree *n3 = axl_ntree_append_data(p, "3");      /* [1,3]     */
    AxlNTree *n2 = axl_ntree_new("2");
    test_check(axl_ntree_insert_after(p, n1, n2) == n2, "ntree: insert_after returns child");
    /* [1,2,3] */
    (void)axl_ntree_prepend_child(p, axl_ntree_new("0"));     /* [0,1,2,3]   */
    (void)axl_ntree_insert_before(p, NULL, axl_ntree_new("4")); /* append [0..4] */
    (void)axl_ntree_insert_before(p, n3, axl_ntree_new("x"));  /* before '3'   */
    char order[64];
    ntree_children_str(p, order);
    test_check(axl_strcmp(order, "012x34") == 0, "ntree: insert ordering 012x34");

    /* A node that already has a parent cannot be re-attached. */
    test_check(axl_ntree_append_child(p, n1) == NULL,
               "ntree: re-parenting an attached node is rejected");

    axl_ntree_free(p);
    axl_ntree_free(root);
}

static void
test_ntree_query_traverse(void)
{
    AxlNTree *root = ntree_build_sample();
    AxlNTree *a  = axl_ntree_first_child(root);
    AxlNTree *a1 = axl_ntree_first_child(a);
    AxlNTree *b  = axl_ntree_nth_child(root, 1);
    AxlNTree *c  = axl_ntree_last_child(root);

    /* Query. */
    test_check(axl_ntree_get_root(a1) == root, "ntree: get_root from a leaf");
    test_check(axl_ntree_depth(root) == 1 && axl_ntree_depth(a) == 2 &&
               axl_ntree_depth(a1) == 3, "ntree: depth is 1-based");
    test_check(axl_ntree_is_ancestor(root, a1) && axl_ntree_is_ancestor(a, a1),
               "ntree: ancestors detected");
    test_check(!axl_ntree_is_ancestor(a1, root) && !axl_ntree_is_ancestor(a, b),
               "ntree: non-ancestors rejected (incl. self/siblings)");
    test_check(axl_ntree_max_height(root) == 3 && axl_ntree_max_height(a) == 2 &&
               axl_ntree_max_height(a1) == 1, "ntree: max_height");
    test_check(axl_ntree_n_nodes(root, AXL_NTREE_ALL) == 7, "ntree: 7 nodes total");
    test_check(axl_ntree_n_nodes(root, AXL_NTREE_LEAVES) == 4, "ntree: 4 leaves");
    test_check(axl_ntree_n_nodes(root, AXL_NTREE_NON_LEAVES) == 3, "ntree: 3 internal");

    /* Traverse orders (exact sequences). */
    NtSeq q;
    q.buf[0] = '\0';
    axl_ntree_traverse(root, AXL_NTREE_PRE_ORDER, AXL_NTREE_ALL, 0, ntree_collect, &q);
    test_check(axl_strcmp(q.buf, "RAA1A2BCC1") == 0, "ntree: pre-order");
    q.buf[0] = '\0';
    axl_ntree_traverse(root, AXL_NTREE_POST_ORDER, AXL_NTREE_ALL, 0, ntree_collect, &q);
    test_check(axl_strcmp(q.buf, "A1A2ABC1CR") == 0, "ntree: post-order");
    q.buf[0] = '\0';
    axl_ntree_traverse(root, AXL_NTREE_LEVEL_ORDER, AXL_NTREE_ALL, 0, ntree_collect, &q);
    test_check(axl_strcmp(q.buf, "RABCA1A2C1") == 0, "ntree: level-order");
    q.buf[0] = '\0';
    axl_ntree_traverse(root, AXL_NTREE_IN_ORDER, AXL_NTREE_ALL, 0, ntree_collect, &q);
    test_check(axl_strcmp(q.buf, "A1AA2RBC1C") == 0, "ntree: in-order");

    /* Flags filter. */
    q.buf[0] = '\0';
    axl_ntree_traverse(root, AXL_NTREE_PRE_ORDER, AXL_NTREE_LEAVES, 0, ntree_collect, &q);
    test_check(axl_strcmp(q.buf, "A1A2BC1") == 0, "ntree: pre-order leaves only");

    /* max_depth. */
    q.buf[0] = '\0';
    axl_ntree_traverse(root, AXL_NTREE_PRE_ORDER, AXL_NTREE_ALL, 1, ntree_collect, &q);
    test_check(axl_strcmp(q.buf, "R") == 0, "ntree: max_depth 1 = root only");
    q.buf[0] = '\0';
    axl_ntree_traverse(root, AXL_NTREE_PRE_ORDER, AXL_NTREE_ALL, 2, ntree_collect, &q);
    test_check(axl_strcmp(q.buf, "RABC") == 0, "ntree: max_depth 2 = root + children");

    /* Early stop. */
    q.buf[0] = '\0';
    axl_ntree_traverse(root, AXL_NTREE_PRE_ORDER, AXL_NTREE_ALL, 0, ntree_collect_stop, &q);
    test_check(axl_strcmp(q.buf, "R") == 0, "ntree: traverse stops on true");

    /* children_foreach with flags. */
    q.buf[0] = '\0';
    axl_ntree_children_foreach(root, AXL_NTREE_ALL, ntree_foreach_collect, &q);
    test_check(axl_strcmp(q.buf, "ABC") == 0, "ntree: children_foreach all");
    q.buf[0] = '\0';
    axl_ntree_children_foreach(root, AXL_NTREE_LEAVES, ntree_foreach_collect, &q);
    test_check(axl_strcmp(q.buf, "B") == 0, "ntree: children_foreach leaves (only 'B')");

    (void)c;
    axl_ntree_free(root);
}

static void
test_ntree_unlink_free(void)
{
    AxlNTree *root = ntree_build_sample();
    AxlNTree *a = axl_ntree_first_child(root);
    AxlNTree *b = axl_ntree_nth_child(root, 1);
    AxlNTree *c = axl_ntree_last_child(root);

    axl_ntree_unlink(b);
    test_check(b->parent == NULL && b->prev == NULL && b->next == NULL,
               "ntree: unlink detaches the node");
    test_check(axl_ntree_n_children(root) == 2, "ntree: unlink drops child count");
    test_check(a->next == c && c->prev == a, "ntree: unlink relinks siblings");
    axl_ntree_unlink(b);   /* idempotent on a root */
    axl_ntree_free(b);

    /* free_full calls the destroy callback once per node in the subtree. */
    ntree_free_calls = 0;
    axl_ntree_free_full(root, ntree_count_free);
    test_check(ntree_free_calls == 6, "ntree: free_full runs destroy per node (6 left)");

    /* NULL-safety: frees are no-ops and queries return zero/NULL. */
    axl_ntree_free(NULL);
    axl_ntree_free_full(NULL, ntree_count_free);
    test_check(axl_ntree_n_children(NULL) == 0 && axl_ntree_depth(NULL) == 0 &&
               axl_ntree_get_root(NULL) == NULL &&
               axl_ntree_max_height(NULL) == 0,
               "ntree: NULL-safe queries return zero/NULL");
}

/* Concatenate iterator-visited node data (pre-order, filtered) into out. */
static void
ntree_iter_str(AxlNTree *root, AxlNTreeTraverseFlags flags, char *out)
{
    AxlNTreeIter it;
    axl_ntree_iter_init(&it, root, flags);
    size_t k = 0;
    for (AxlNTree *n; (n = axl_ntree_iter_next(&it)) != NULL; ) {
        for (const char *s = n->data; *s != '\0'; s++) {
            out[k++] = *s;
        }
    }
    out[k] = '\0';
}

static void
test_ntree_iter(void)
{
    AxlNTree *root = ntree_build_sample();
    char buf[64];

    ntree_iter_str(root, AXL_NTREE_ALL, buf);
    test_check(axl_strcmp(buf, "RAA1A2BCC1") == 0, "ntree_iter: pre-order all");
    ntree_iter_str(root, AXL_NTREE_LEAVES, buf);
    test_check(axl_strcmp(buf, "A1A2BC1") == 0, "ntree_iter: leaves only");
    ntree_iter_str(root, AXL_NTREE_NON_LEAVES, buf);
    test_check(axl_strcmp(buf, "RAC") == 0, "ntree_iter: internal only");

    /* Single-node subtree. */
    AxlNTree *solo = axl_ntree_new("S");
    ntree_iter_str(solo, AXL_NTREE_ALL, buf);
    test_check(axl_strcmp(buf, "S") == 0, "ntree_iter: single node");

    /* Iterating from a non-root node stays inside that subtree. */
    ntree_iter_str(axl_ntree_first_child(root), AXL_NTREE_ALL, buf);
    test_check(axl_strcmp(buf, "AA1A2") == 0, "ntree_iter: bounded to subtree of 'A'");

    /* NULL root -> immediately exhausted. */
    AxlNTreeIter it;
    axl_ntree_iter_init(&it, NULL, AXL_NTREE_ALL);
    test_check(axl_ntree_iter_next(&it) == NULL, "ntree_iter: NULL root exhausted");

    axl_ntree_free(solo);
    axl_ntree_free(root);
}

/* Concatenate REVERSE-pre-order iterator-visited data (filtered) into out. */
static void
ntree_iter_rev_str(AxlNTree *root, AxlNTreeTraverseFlags flags, char *out)
{
    AxlNTreeIter it;
    axl_ntree_iter_init_reverse(&it, root, flags);
    size_t k = 0;
    for (AxlNTree *n; (n = axl_ntree_iter_next(&it)) != NULL; ) {
        for (const char *s = n->data; *s != '\0'; s++) {
            out[k++] = *s;
        }
    }
    out[k] = '\0';
}

static void
test_ntree_iter_reverse(void)
{
    AxlNTree *root = ntree_build_sample();  /* R->[A->[A1,A2], B, C->[C1]] */
    char buf[64];

    /* Exact reverse of forward pre-order "RAA1A2BCC1". */
    ntree_iter_rev_str(root, AXL_NTREE_ALL, buf);
    test_check(axl_strcmp(buf, "C1CBA2A1AR") == 0,
               "ntree_iter_rev: reverse pre-order all (topmost-first)");
    ntree_iter_rev_str(root, AXL_NTREE_LEAVES, buf);
    test_check(axl_strcmp(buf, "C1BA2A1") == 0, "ntree_iter_rev: leaves reversed");
    ntree_iter_rev_str(root, AXL_NTREE_NON_LEAVES, buf);
    test_check(axl_strcmp(buf, "CAR") == 0, "ntree_iter_rev: internal reversed");

    /* Bounded to a subtree, and the single-node / NULL cases. */
    ntree_iter_rev_str(axl_ntree_first_child(root), AXL_NTREE_ALL, buf);
    test_check(axl_strcmp(buf, "A2A1A") == 0, "ntree_iter_rev: bounded to subtree of 'A'");
    AxlNTree *solo = axl_ntree_new("S");
    ntree_iter_rev_str(solo, AXL_NTREE_ALL, buf);
    test_check(axl_strcmp(buf, "S") == 0, "ntree_iter_rev: single node");
    AxlNTreeIter it;
    axl_ntree_iter_init_reverse(&it, NULL, AXL_NTREE_ALL);
    test_check(axl_ntree_iter_next(&it) == NULL, "ntree_iter_rev: NULL root exhausted");

    axl_ntree_free(solo);
    axl_ntree_free(root);
}

static void
test_ntree_move(void)
{
    char buf[64];

    /* place-above / place-below a sibling. */
    AxlNTree *r = axl_ntree_new("R");
    AxlNTree *a = axl_ntree_append_data(r, "A");
    (void)axl_ntree_append_data(r, "B");
    AxlNTree *c = axl_ntree_append_data(r, "C");        /* R->[A,B,C] */
    test_check(axl_ntree_move_after(r, a, c) == c, "ntree_move: after returns node");
    ntree_children_str(r, buf);
    test_check(axl_strcmp(buf, "ACB") == 0, "ntree_move: move_after(A) -> A,C,B");
    (void)axl_ntree_move_before(r, a, c);
    ntree_children_str(r, buf);
    test_check(axl_strcmp(buf, "CAB") == 0, "ntree_move: move_before(A) -> C,A,B");

    /* NULL sibling: after = first, before = last. */
    (void)axl_ntree_move_before(r, NULL, c);            /* C to last */
    ntree_children_str(r, buf);
    test_check(axl_strcmp(buf, "ABC") == 0, "ntree_move: move_before(NULL) -> last");
    (void)axl_ntree_move_after(r, NULL, c);             /* C to first */
    ntree_children_str(r, buf);
    test_check(axl_strcmp(buf, "CAB") == 0, "ntree_move: move_after(NULL) -> first");
    axl_ntree_free(r);

    /* Reparent: move B from under R to under A. */
    AxlNTree *r2 = axl_ntree_new("R");
    AxlNTree *a2 = axl_ntree_append_data(r2, "A");
    AxlNTree *b2 = axl_ntree_append_data(r2, "B");
    (void)axl_ntree_append_data(a2, "A1");              /* R->[A->[A1], B] */
    test_check(axl_ntree_move_after(a2, NULL, b2) == b2, "ntree_move: reparent returns node");
    test_check(b2->parent == a2, "ntree_move: reparent sets parent");
    ntree_children_str(r2, buf);
    test_check(axl_strcmp(buf, "A") == 0, "ntree_move: reparent removed B from R");
    ntree_children_str(a2, buf);
    test_check(axl_strcmp(buf, "BA1") == 0, "ntree_move: reparent put B first under A");

    /* Cycle / bad-arg rejection — tree must be untouched. */
    AxlNTree *a1 = axl_ntree_first_child(a2)->next;     /* A1 (B is first now) */
    test_check(a1 != NULL && axl_strcmp(a1->data, "A1") == 0, "ntree_move: located A1");
    test_check(axl_ntree_move_after(a1, NULL, a2) == NULL,
               "ntree_move: rejects cycle (A under its descendant A1)");
    test_check(axl_ntree_move_after(a2, NULL, a2) == NULL, "ntree_move: rejects node==parent");
    test_check(axl_ntree_move_after(r2, b2, b2) == NULL, "ntree_move: rejects node==sibling");
    test_check(axl_ntree_move_after(r2, a1, b2) == NULL,
               "ntree_move: rejects sibling not a child of parent");
    test_check(a2->parent == r2 && b2->parent == a2, "ntree_move: tree intact after rejects");
    ntree_children_str(a2, buf);
    test_check(axl_strcmp(buf, "BA1") == 0, "ntree_move: sibling order intact after rejects");

    axl_ntree_free(r2);
}

// ---------------------------------------------------------------------------
// AxlTree (AVL sorted map) Tests
// ---------------------------------------------------------------------------

static int
tree_cmp_intptr(const void *a, const void *b, void *user)
{
    (void)user;
    intptr_t x = (intptr_t)a, y = (intptr_t)b;
    return (x > y) - (x < y);
}

static int
tree_cmp_pint(const void *a, const void *b, void *user)
{
    (void)user;
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

typedef struct { intptr_t prev; bool ok; uint32_t count; } TreeOrderCtx;

static bool
tree_order_check(void *key, void *value, void *user)
{
    (void)value;
    TreeOrderCtx *c = user;
    intptr_t k = (intptr_t)key;
    if (c->count > 0 && k <= c->prev) {
        c->ok = false;
    }
    c->prev = k;
    c->count++;
    return false;
}

/* Ceil log2 helper for the AVL height bound. */
static uint32_t
ceil_log2(uint32_t n)
{
    uint32_t bits = 0;
    while ((1u << bits) < n) {
        bits++;
    }
    return bits;
}

static void
test_tree_basic(void)
{
    AxlTree *t = axl_tree_new(tree_cmp_intptr, NULL);
    test_check(t != NULL && axl_tree_nnodes(t) == 0, "tree: new is empty");

    axl_tree_insert(t, (void *)3, (void *)30);
    axl_tree_insert(t, (void *)1, (void *)10);
    axl_tree_insert(t, (void *)2, (void *)20);
    test_check(axl_tree_nnodes(t) == 3, "tree: 3 nodes after inserts");
    test_check((intptr_t)axl_tree_lookup(t, (void *)2) == 20, "tree: lookup hit");
    test_check(axl_tree_lookup(t, (void *)9) == NULL, "tree: lookup miss is NULL");

    void *fk = NULL, *fv = NULL;
    test_check(axl_tree_lookup_extended(t, (void *)1, &fk, &fv) &&
               (intptr_t)fk == 1 && (intptr_t)fv == 10,
               "tree: lookup_extended reports key+value");
    test_check(!axl_tree_lookup_extended(t, (void *)9, &fk, &fv),
               "tree: lookup_extended miss returns false");

    /* foreach visits in ascending key order. */
    TreeOrderCtx ctx = { 0, true, 0 };
    axl_tree_foreach(t, tree_order_check, &ctx);
    test_check(ctx.ok && ctx.count == 3, "tree: foreach is in ascending order");

    /* insert of an existing key replaces the value, keeps node count. */
    axl_tree_insert(t, (void *)2, (void *)99);
    test_check(axl_tree_nnodes(t) == 3 &&
               (intptr_t)axl_tree_lookup(t, (void *)2) == 99,
               "tree: re-insert replaces value, not count");

    /* NULL-safety. */
    test_check(axl_tree_lookup(NULL, (void *)1) == NULL &&
               axl_tree_nnodes(NULL) == 0 && axl_tree_height(NULL) == 0 &&
               !axl_tree_remove(NULL, (void *)1),
               "tree: NULL-safe queries");
    axl_tree_free(NULL);
    axl_tree_free(t);
}

static void
test_tree_balance(void)
{
    const intptr_t N = 1000;
    /* Tight AVL guard: max height < 1.5*log2(n) + 2 (AVL's true bound is
     * ~1.44*log2(n+2)); a degenerate or mis-rotated tree blows past it. */
    uint32_t bound = ceil_log2((uint32_t)N + 1u) * 3u / 2u + 2u;

    /* Three pathological insert orders that would unbalance a plain BST. */
    for (int mode = 0; mode < 3; mode++) {
        AxlTree *t = axl_tree_new(tree_cmp_intptr, NULL);
        for (intptr_t i = 0; i < N; i++) {
            intptr_t key;
            if (mode == 0) { key = i + 1; }                       /* ascending  */
            else if (mode == 1) { key = N - i; }                  /* descending */
            else { key = (i & 1) ? (N - i / 2) : (i / 2 + 1); }   /* zig-zag    */
            axl_tree_insert(t, (void *)key, (void *)(key * 2));
        }
        test_check(axl_tree_nnodes(t) == (uint32_t)N,
                   "tree: all stress keys inserted");
        test_check(axl_tree_height(t) <= bound,
                   "tree: AVL height stays O(log n) under adversarial order");

        bool all_found = true;
        for (intptr_t i = 1; i <= N; i++) {
            if ((intptr_t)axl_tree_lookup(t, (void *)i) != i * 2) {
                all_found = false;
            }
        }
        test_check(all_found, "tree: every stress key looks up correctly");

        TreeOrderCtx ctx = { 0, true, 0 };
        axl_tree_foreach(t, tree_order_check, &ctx);
        test_check(ctx.ok && ctx.count == (uint32_t)N,
                   "tree: stress tree iterates fully sorted");
        axl_tree_free(t);
    }
}

static void
test_tree_remove(void)
{
    AxlTree *t = axl_tree_new(tree_cmp_intptr, NULL);
    for (intptr_t i = 1; i <= 15; i++) {
        axl_tree_insert(t, (void *)i, (void *)(i * 10));
    }

    /* Remove a leaf, a one-child node, and a two-child node (the exact
     * shapes depend on AVL balancing, but all three configurations are
     * exercised across these removals of a 15-node tree). */
    test_check(axl_tree_remove(t, (void *)1), "tree: remove leaf");
    test_check(axl_tree_remove(t, (void *)8), "tree: remove internal (two-child)");
    test_check(axl_tree_remove(t, (void *)15), "tree: remove edge");
    test_check(!axl_tree_remove(t, (void *)8), "tree: re-remove returns false");
    test_check(axl_tree_nnodes(t) == 12, "tree: node count after 3 removals");
    test_check(axl_tree_lookup(t, (void *)8) == NULL, "tree: removed key gone");
    test_check((intptr_t)axl_tree_lookup(t, (void *)7) == 70, "tree: neighbors intact");

    /* Tree stays sorted + balanced after removals. */
    uint32_t bound = ceil_log2(16u) * 3u / 2u + 2u;
    test_check(axl_tree_height(t) <= bound, "tree: balanced after removals");
    TreeOrderCtx ctx = { 0, true, 0 };
    axl_tree_foreach(t, tree_order_check, &ctx);
    test_check(ctx.ok && ctx.count == 12, "tree: sorted order preserved after removals");

    /* Drain the rest. */
    for (intptr_t i = 2; i <= 14; i++) {
        if (i != 8) {
            (void)axl_tree_remove(t, (void *)i);
        }
    }
    test_check(axl_tree_nnodes(t) == 0 && axl_tree_height(t) == 0,
               "tree: emptied to zero nodes / height");
    axl_tree_free(t);

    /* Delete-heavy stress: insert 1..500, remove every even key (exercises
     * the delete-path rotations at scale), then verify the survivors stay
     * balanced, sorted, and fully present. */
    AxlTree *s = axl_tree_new(tree_cmp_intptr, NULL);
    for (intptr_t i = 1; i <= 500; i++) {
        axl_tree_insert(s, (void *)i, (void *)(i * 3));
    }
    for (intptr_t i = 2; i <= 500; i += 2) {
        (void)axl_tree_remove(s, (void *)i);
    }
    test_check(axl_tree_nnodes(s) == 250, "tree: 250 odd keys remain after deletes");
    test_check(axl_tree_height(s) <= ceil_log2(251u) * 3u / 2u + 2u,
               "tree: balanced after 250 deletes");
    bool odds_ok = true;
    for (intptr_t i = 1; i <= 500; i++) {
        void *v = axl_tree_lookup(s, (void *)i);
        if (i & 1) {
            if ((intptr_t)v != i * 3) { odds_ok = false; }
        } else if (v != NULL) {
            odds_ok = false;
        }
    }
    test_check(odds_ok, "tree: odds present, evens gone after delete stress");
    TreeOrderCtx sctx = { 0, true, 0 };
    axl_tree_foreach(s, tree_order_check, &sctx);
    test_check(sctx.ok && sctx.count == 250, "tree: sorted after delete stress");
    axl_tree_free(s);
}

static int tree_key_frees;
static int tree_val_frees;
static void tree_key_free(void *k) { tree_key_frees++; axl_free(k); }
static void tree_val_free(void *v) { tree_val_frees++; axl_free(v); }

static int *
heap_int(int v)
{
    int *p = axl_malloc(sizeof(int));
    if (p != NULL) {
        *p = v;
    }
    return p;
}

static void
test_tree_insert_replace_destroy(void)
{
    tree_key_frees = 0;
    tree_val_frees = 0;
    AxlTree *t = axl_tree_new_full(tree_cmp_pint, NULL, tree_key_free, tree_val_free);

    axl_tree_insert(t, heap_int(5), heap_int(100));   /* new entry */
    test_check(tree_key_frees == 0 && tree_val_frees == 0,
               "tree: first insert frees nothing");

    /* insert on collision: keep OLD key (free the NEW key), replace value
     * (free the OLD value). */
    int probe = 5;
    axl_tree_insert(t, heap_int(5), heap_int(200));
    test_check(tree_key_frees == 1 && tree_val_frees == 1,
               "tree: insert collision frees new key + old value");
    test_check(*(int *)axl_tree_lookup(t, &probe) == 200,
               "tree: insert collision updated the value");

    /* replace on collision: free OLD key AND old value. */
    axl_tree_replace(t, heap_int(5), heap_int(300));
    test_check(tree_key_frees == 2 && tree_val_frees == 2,
               "tree: replace collision frees old key + old value");

    /* remove frees the surviving key + value. */
    test_check(axl_tree_remove(t, &probe), "tree: remove found the entry");
    test_check(tree_key_frees == 3 && tree_val_frees == 3,
               "tree: remove frees key + value");

    /* free drains remaining entries through the destructors. */
    axl_tree_insert(t, heap_int(1), heap_int(10));
    axl_tree_insert(t, heap_int(2), heap_int(20));
    axl_tree_free(t);
    test_check(tree_key_frees == 5 && tree_val_frees == 5,
               "tree: free runs destructors on every remaining entry");
}

static void
test_tree_bounds(void)
{
    AxlTree *t = axl_tree_new(tree_cmp_intptr, NULL);
    axl_tree_insert(t, (void *)10, (void *)100);
    axl_tree_insert(t, (void *)20, (void *)200);
    axl_tree_insert(t, (void *)30, (void *)300);
    axl_tree_insert(t, (void *)40, (void *)400);

    test_check((intptr_t)axl_tree_lower_bound(t, (void *)20) == 200,
               "tree: lower_bound exact key");
    test_check((intptr_t)axl_tree_lower_bound(t, (void *)25) == 300,
               "tree: lower_bound rounds up to 30");
    test_check((intptr_t)axl_tree_lower_bound(t, (void *)5) == 100,
               "tree: lower_bound below all -> smallest");
    test_check(axl_tree_lower_bound(t, (void *)45) == NULL,
               "tree: lower_bound above all -> NULL");

    test_check((intptr_t)axl_tree_upper_bound(t, (void *)20) == 300,
               "tree: upper_bound is strictly greater");
    test_check((intptr_t)axl_tree_upper_bound(t, (void *)25) == 300,
               "tree: upper_bound of gap -> 30");
    test_check(axl_tree_upper_bound(t, (void *)40) == NULL,
               "tree: upper_bound of max -> NULL");
    axl_tree_free(t);

    /* Empty (non-NULL) tree: both bounds are NULL. */
    AxlTree *e = axl_tree_new(tree_cmp_intptr, NULL);
    test_check(axl_tree_lower_bound(e, (void *)1) == NULL &&
               axl_tree_upper_bound(e, (void *)1) == NULL,
               "tree: bounds on empty tree are NULL");
    axl_tree_free(e);
}

static void
test_tree_iter(void)
{
    AxlTree *t = axl_tree_new(tree_cmp_intptr, NULL);
    intptr_t ins[] = { 5, 1, 3, 2, 4 };
    for (int i = 0; i < 5; i++) {
        axl_tree_insert(t, (void *)ins[i], (void *)(ins[i] * 10));
    }

    AxlTreeIter it;
    void *k, *v;
    axl_tree_iter_init(&it, t);
    intptr_t expect = 1;
    bool ok = true;
    int count = 0;
    while (axl_tree_iter_next(&it, &k, &v)) {
        if ((intptr_t)k != expect || (intptr_t)v != expect * 10) {
            ok = false;
        }
        expect++;
        count++;
    }
    test_check(ok && count == 5, "tree_iter: ascending key+value, full count");

    /* NULL out-params are tolerated. */
    axl_tree_iter_init(&it, t);
    count = 0;
    while (axl_tree_iter_next(&it, NULL, NULL)) {
        count++;
    }
    test_check(count == 5, "tree_iter: NULL out-params tolerated");

    /* Empty and NULL trees iterate to nothing. */
    AxlTree *e = axl_tree_new(tree_cmp_intptr, NULL);
    axl_tree_iter_init(&it, e);
    test_check(!axl_tree_iter_next(&it, &k, &v), "tree_iter: empty tree exhausted");
    axl_tree_iter_init(&it, NULL);
    test_check(!axl_tree_iter_next(&it, &k, &v), "tree_iter: NULL tree exhausted");
    axl_tree_free(e);
    axl_tree_free(t);

    /* Deep stack: 1000 nodes inserted descending still iterate ascending. */
    AxlTree *big = axl_tree_new(tree_cmp_intptr, NULL);
    for (intptr_t i = 1000; i >= 1; i--) {
        axl_tree_insert(big, (void *)i, (void *)i);
    }
    axl_tree_iter_init(&it, big);
    expect = 1;
    ok = true;
    count = 0;
    while (axl_tree_iter_next(&it, &k, NULL)) {
        if ((intptr_t)k != expect) {
            ok = false;
        }
        expect++;
        count++;
    }
    test_check(ok && count == 1000, "tree_iter: 1000-node deep iteration is sorted");
    axl_tree_free(big);
}

// ---------------------------------------------------------------------------
// Ring Buffer Tests
// ---------------------------------------------------------------------------

static void
test_ring_buf(void)
{
    AxlRingBuf *rb = axl_ring_buf_new(100);
    test_check(rb != NULL, "ring: new");
    if (rb == NULL) {
        return;
    }

    /* Capacity rounded up to power of 2 */
    test_check(axl_ring_buf_get_capacity(rb) == 128, "ring: capacity 128 (rounded)");
    test_check(axl_ring_buf_is_empty(rb), "ring: initially empty");
    test_check(axl_ring_buf_get_readable(rb) == 0, "ring: readable 0");
    test_check(axl_ring_buf_get_writable(rb) == 128, "ring: writable 128");

    /* Write and read */
    uint32_t n = axl_ring_buf_push(rb, "hello", 5);
    test_check(n == 5, "ring: write 5");
    test_check(axl_ring_buf_get_readable(rb) == 5, "ring: readable 5");
    test_check(!axl_ring_buf_is_empty(rb), "ring: not empty");

    char buf[32];
    n = axl_ring_buf_pop(rb, buf, sizeof(buf));
    test_check(n == 5, "ring: read 5");
    test_check(axl_memcmp(buf, "hello", 5) == 0, "ring: read data matches");
    test_check(axl_ring_buf_is_empty(rb), "ring: empty after read");

    /* Clear */
    axl_ring_buf_push(rb, "data", 4);
    axl_ring_buf_clear(rb);
    test_check(axl_ring_buf_is_empty(rb), "ring: empty after clear");

    /* NULL safety */
    test_check(axl_ring_buf_new(0) == NULL, "ring: new(0) returns NULL");
    test_check(axl_ring_buf_get_readable(NULL) == 0, "ring: readable(NULL)");
    axl_ring_buf_free(NULL);
    test_check(true, "ring: free(NULL) no crash");

    axl_ring_buf_free(rb);
}

static void
test_ring_buf_wrap(void)
{
    AxlRingBuf *rb = axl_ring_buf_new(8);
    test_check(axl_ring_buf_get_capacity(rb) == 8, "ring_wrap: capacity 8");

    /* Fill most of the buffer */
    axl_ring_buf_push(rb, "ABCDEF", 6);
    /* Consume 4, leaving "EF" */
    axl_ring_buf_discard(rb, 4);
    test_check(axl_ring_buf_get_readable(rb) == 2, "ring_wrap: 2 readable");

    /* Write 5 more — wraps around the end */
    uint32_t n = axl_ring_buf_push(rb, "12345", 5);
    test_check(n == 5, "ring_wrap: write 5 wrapping");
    test_check(axl_ring_buf_get_readable(rb) == 7, "ring_wrap: 7 readable");

    /* Read all — should get "EF12345" */
    char buf[8];
    n = axl_ring_buf_pop(rb, buf, 7);
    test_check(n == 7, "ring_wrap: read 7");
    test_check(axl_memcmp(buf, "EF12345", 7) == 0, "ring_wrap: data correct");

    axl_ring_buf_free(rb);
}

static void
test_ring_buf_overwrite(void)
{
    AxlRingBuf *rb = axl_ring_buf_new_full(8, AXL_RING_BUF_OVERWRITE);
    test_check(rb != NULL, "ring_ow: new");

    /* Fill buffer completely */
    axl_ring_buf_push(rb, "ABCDEFGH", 8);
    test_check(axl_ring_buf_is_full(rb), "ring_ow: full");

    /* Write more — oldest data discarded */
    axl_ring_buf_push(rb, "XY", 2);
    test_check(axl_ring_buf_get_readable(rb) == 8, "ring_ow: still 8 readable");

    char buf[8];
    axl_ring_buf_pop(rb, buf, 8);
    test_check(axl_memcmp(buf, "CDEFGHXY", 8) == 0,
               "ring_ow: oldest discarded");

    /* Write more than capacity — only last 8 bytes kept */
    axl_ring_buf_push(rb, "0123456789AB", 12);
    test_check(axl_ring_buf_get_readable(rb) == 8, "ring_ow: capped at capacity");
    axl_ring_buf_pop(rb, buf, 8);
    test_check(axl_memcmp(buf, "456789AB", 8) == 0,
               "ring_ow: only tail kept");

    axl_ring_buf_free(rb);
}

static void
test_ring_buf_peek_discard(void)
{
    AxlRingBuf *rb = axl_ring_buf_new(16);
    axl_ring_buf_push(rb, "hello world", 11);

    /* Peek doesn't consume */
    char buf[16];
    uint32_t n = axl_ring_buf_peek(rb, buf, 5);
    test_check(n == 5, "ring_peek: got 5");
    test_check(axl_memcmp(buf, "hello", 5) == 0, "ring_peek: data");
    test_check(axl_ring_buf_get_readable(rb) == 11, "ring_peek: still 11");

    /* Discard consumes without reading */
    n = axl_ring_buf_discard(rb, 6);
    test_check(n == 6, "ring_discard: 6");
    test_check(axl_ring_buf_get_readable(rb) == 5, "ring_discard: 5 left");

    n = axl_ring_buf_pop(rb, buf, 5);
    test_check(axl_memcmp(buf, "world", 5) == 0, "ring_discard: remaining");

    axl_ring_buf_free(rb);
}

static void
test_ring_buf_regions(void)
{
    AxlRingBuf *rb = axl_ring_buf_new(8);
    AxlRingBufRegion regions[2];
    uint32_t count;

    /* Empty: 0 regions */
    count = axl_ring_buf_peek_regions(rb, regions);
    test_check(count == 0, "ring_regions: empty = 0");

    /* Non-wrapping: 1 region */
    axl_ring_buf_push(rb, "ABC", 3);
    count = axl_ring_buf_peek_regions(rb, regions);
    test_check(count == 1, "ring_regions: contiguous = 1");
    test_check(regions[0].len == 3, "ring_regions: len 3");
    test_check(axl_memcmp(regions[0].data, "ABC", 3) == 0,
               "ring_regions: data");

    /* Force wrap: consume 6, write 7 more */
    axl_ring_buf_discard(rb, 3);
    axl_ring_buf_push(rb, "DEFGHI", 6);
    axl_ring_buf_discard(rb, 3);
    axl_ring_buf_push(rb, "JKL", 3);

    count = axl_ring_buf_peek_regions(rb, regions);
    test_check(count == 2, "ring_regions: wrap = 2");
    test_check(regions[0].len + regions[1].len ==
               axl_ring_buf_get_readable(rb), "ring_regions: total len");

    /* Write regions for zero-copy write */
    axl_ring_buf_clear(rb);
    count = axl_ring_buf_push_regions(rb, regions);
    test_check(count >= 1, "ring_wregions: non-empty");
    test_check(regions[0].len == 8, "ring_wregions: full capacity");

    /* Zero-copy write + advance */
    axl_memcpy(regions[0].data, "ZERO", 4);
    axl_ring_buf_push_advance(rb, 4);
    test_check(axl_ring_buf_get_readable(rb) == 4, "ring_wadvance: 4 readable");

    char buf[4];
    axl_ring_buf_pop(rb, buf, 4);
    test_check(axl_memcmp(buf, "ZERO", 4) == 0, "ring_wadvance: data");

    axl_ring_buf_free(rb);
}

static void
test_ring_buf_partial(void)
{
    AxlRingBuf *rb = axl_ring_buf_new(4);
    test_check(axl_ring_buf_get_capacity(rb) == 4, "ring_partial: cap 4");

    /* Write exactly fills */
    uint32_t n = axl_ring_buf_push(rb, "ABCD", 4);
    test_check(n == 4, "ring_partial: write 4");
    test_check(axl_ring_buf_is_full(rb), "ring_partial: full");

    /* Reject mode: additional write returns 0 */
    n = axl_ring_buf_push(rb, "X", 1);
    test_check(n == 0, "ring_partial: reject when full");

    /* Partial write */
    axl_ring_buf_discard(rb, 2);
    n = axl_ring_buf_push(rb, "XYZ", 3);
    test_check(n == 2, "ring_partial: partial write 2/3");

    /* Partial read */
    char buf[8];
    n = axl_ring_buf_pop(rb, buf, 8);
    test_check(n == 4, "ring_partial: partial read 4/8");

    axl_ring_buf_free(rb);
}

static void
test_ring_buf_push_stats(void)
{
    /* Reject mode (default): rejected bytes go into pushes_lost. */
    AxlRingBuf *rb = axl_ring_buf_new(4);
    test_check(axl_ring_buf_pushes_total(rb) == 0,
               "ring_stats: pushes_total starts at 0");
    test_check(axl_ring_buf_pushes_lost(rb) == 0,
               "ring_stats: pushes_lost starts at 0");

    axl_ring_buf_push(rb, "ABCD", 4);
    test_check(axl_ring_buf_pushes_total(rb) == 4,
               "ring_stats: total 4 after push 4");
    test_check(axl_ring_buf_pushes_lost(rb) == 0,
               "ring_stats: lost 0 when buffer fits");

    /* Reject path: 3 bytes asked, 0 bytes fit. */
    axl_ring_buf_push(rb, "XYZ", 3);
    test_check(axl_ring_buf_pushes_total(rb) == 7,
               "ring_stats: total counts rejected attempts");
    test_check(axl_ring_buf_pushes_lost(rb) == 3,
               "ring_stats: lost counts rejected bytes");

    /* Partial reject: 1 free, 3 pushed → 2 lost. */
    axl_ring_buf_discard(rb, 1);
    axl_ring_buf_push(rb, "PQR", 3);
    test_check(axl_ring_buf_pushes_total(rb) == 10,
               "ring_stats: total counts partial-reject attempt");
    test_check(axl_ring_buf_pushes_lost(rb) == 5,
               "ring_stats: lost counts partial-rejected bytes");

    axl_ring_buf_free(rb);

    /* Overwrite mode: displaced old bytes count as lost. */
    rb = axl_ring_buf_new_full(4, AXL_RING_BUF_OVERWRITE);
    axl_ring_buf_push(rb, "ABCD", 4);
    axl_ring_buf_push(rb, "12", 2);   /* displaces 2 old bytes */
    test_check(axl_ring_buf_pushes_total(rb) == 6,
               "ring_stats: overwrite total");
    test_check(axl_ring_buf_pushes_lost(rb) == 2,
               "ring_stats: overwrite displaces 2 old bytes");

    /* Oversized overwrite push: input dropped + old displaced. */
    axl_ring_buf_push(rb, "abcdefgh", 8);
    /* Ring size is 4. orig_len=8: input_dropped=4 (first 4 bytes
     * of input dropped), only last 4 written. Before this push,
     * buffer was full (CD12). Those 4 are all displaced. Lost
     * accumulates: 2 (from previous step) + 4 (input dropped) +
     * 4 (displaced old) = 10. */
    test_check(axl_ring_buf_pushes_total(rb) == 14,
               "ring_stats: oversized overwrite total counts orig_len");
    test_check(axl_ring_buf_pushes_lost(rb) == 10,
               "ring_stats: oversized overwrite lost counts dropped+displaced");

    /* clear() resets the counters. */
    axl_ring_buf_clear(rb);
    test_check(axl_ring_buf_pushes_total(rb) == 0,
               "ring_stats: clear resets pushes_total");
    test_check(axl_ring_buf_pushes_lost(rb) == 0,
               "ring_stats: clear resets pushes_lost");

    axl_ring_buf_free(rb);

    /* Element mode (the reqlog-shape consumer). */
    AxlRingBuf rb_elem;
    uint32_t storage[8];   /* 32 bytes, pow2 */
    axl_ring_buf_init_fixed(&rb_elem, storage, sizeof(storage),
                            sizeof(uint32_t),
                            AXL_RING_BUF_OVERWRITE, NULL);
    for (uint32_t v = 0; v < 12; v++) {
        axl_ring_buf_push_elem(&rb_elem, &v);
    }
    /* 12 elements pushed * 4 bytes = 48 attempted; 8-elem capacity
     * means 4 elements were displaced = 16 bytes lost. */
    test_check(axl_ring_buf_pushes_total(&rb_elem) == 12 * sizeof(uint32_t),
               "ring_stats: elem total = pushes * elem_size");
    test_check(axl_ring_buf_pushes_lost(&rb_elem) == 4 * sizeof(uint32_t),
               "ring_stats: elem lost = displaced * elem_size");

    /* Reject elem: a full ring rejects new elements; rejected count
     * goes into pushes_lost. */
    AxlRingBuf rb_reject;
    uint32_t storage2[4];
    axl_ring_buf_init_fixed(&rb_reject, storage2, sizeof(storage2),
                            sizeof(uint32_t), 0, NULL);
    for (uint32_t v = 0; v < 4; v++) {
        axl_ring_buf_push_elem(&rb_reject, &v);
    }
    /* Now full. Try one more — must reject and count as lost. */
    uint32_t reject_val = 99;
    int rc = axl_ring_buf_push_elem(&rb_reject, &reject_val);
    test_check(rc == AXL_ERR, "ring_stats: full elem ring rejects");
    test_check(axl_ring_buf_pushes_total(&rb_reject) == 5 * sizeof(uint32_t),
               "ring_stats: reject elem counts in total");
    test_check(axl_ring_buf_pushes_lost(&rb_reject) == sizeof(uint32_t),
               "ring_stats: reject elem counts elem_size in lost");
}

static void
test_ring_buf_power_of_2(void)
{
    AxlRingBuf *rb;

    rb = axl_ring_buf_new(1);
    test_check(axl_ring_buf_get_capacity(rb) == 1, "ring_pow2: 1 -> 1");
    axl_ring_buf_free(rb);

    rb = axl_ring_buf_new(3);
    test_check(axl_ring_buf_get_capacity(rb) == 4, "ring_pow2: 3 -> 4");
    axl_ring_buf_free(rb);

    rb = axl_ring_buf_new(5);
    test_check(axl_ring_buf_get_capacity(rb) == 8, "ring_pow2: 5 -> 8");
    axl_ring_buf_free(rb);

    rb = axl_ring_buf_new(256);
    test_check(axl_ring_buf_get_capacity(rb) == 256, "ring_pow2: 256 -> 256");
    axl_ring_buf_free(rb);

    rb = axl_ring_buf_new(1000);
    test_check(axl_ring_buf_get_capacity(rb) == 1024, "ring_pow2: 1000 -> 1024");
    axl_ring_buf_free(rb);
}

static void
test_ring_buf_init(void)
{
    /* Stack-allocated struct + buffer */
    uint8_t buf[64];
    AxlRingBuf rb;

    test_check(axl_ring_buf_init(&rb, buf, 64, 0, NULL) == AXL_OK, "ring_init: ok");
    test_check(axl_ring_buf_get_capacity(&rb) == 64, "ring_init: capacity 64");
    test_check(axl_ring_buf_is_empty(&rb), "ring_init: empty");

    /* Write and read via stack buffer */
    axl_ring_buf_push(&rb, "test", 4);
    test_check(axl_ring_buf_get_readable(&rb) == 4, "ring_init: wrote 4");

    char out[8];
    axl_ring_buf_pop(&rb, out, 4);
    test_check(axl_memcmp(out, "test", 4) == 0, "ring_init: read ok");

    /* Deinit with NULL buf_free doesn't free caller's buffer */
    axl_ring_buf_deinit(&rb);
    test_check(buf[0] == 't', "ring_init: buf intact after deinit");

    /* Deinit with buf_free calls the deallocator */
    radix_free_count = 0;
    uint8_t heap_buf[64];
    axl_ring_buf_init(&rb, heap_buf, 64, 0, radix_free_counter);
    axl_ring_buf_push(&rb, "data", 4);
    axl_ring_buf_deinit(&rb);
    test_check(radix_free_count == 1, "ring_init: buf_free called on deinit");

    /* Non-power-of-2 rejected */
    test_check(axl_ring_buf_init(&rb, buf, 50, 0, NULL) == AXL_ERR,
               "ring_init: reject non-pow2");

    /* NULL args rejected */
    test_check(axl_ring_buf_init(NULL, buf, 64, 0, NULL) == AXL_ERR,
               "ring_init: NULL rb");
    test_check(axl_ring_buf_init(&rb, NULL, 64, 0, NULL) == AXL_ERR,
               "ring_init: NULL buf");

    /* init_fixed rejects elem_size 0 */
    test_check(axl_ring_buf_init_fixed(&rb, buf, 64, 0, 0, NULL) == AXL_ERR,
               "ring_init: init_fixed(0) rejected");

    /* new_fixed rejects elem_size 0 */
    test_check(axl_ring_buf_new_fixed(64, 0, 0) == NULL,
               "ring_init: new_fixed(0) NULL");

    /* Layer 3 functions rejected on byte-mode buffer */
    axl_ring_buf_init(&rb, buf, 64, 0, NULL);
    int dummy = 42;
    test_check(axl_ring_buf_push_elem(&rb, &dummy) == AXL_ERR,
               "ring_init: push_elem byte-mode rejected");
    test_check(axl_ring_buf_pop_elem(&rb, &dummy) == AXL_ERR,
               "ring_init: pop_elem byte-mode rejected");
    axl_ring_buf_deinit(&rb);

    /* get_length in byte mode returns byte count */
    axl_ring_buf_init(&rb, buf, 64, 0, NULL);
    axl_ring_buf_push(&rb, "hello", 5);
    test_check(axl_ring_buf_get_length(&rb) == 5,
               "ring_init: get_length byte mode");
    axl_ring_buf_deinit(&rb);
}

static void
test_ring_buf_user_buffer(void)
{
    /* new_with_buffer: heap struct, caller's buffer */
    uint8_t buf[32];
    AxlRingBuf *rb = axl_ring_buf_new_with_buffer(buf, 32, 0);
    test_check(rb != NULL, "ring_userbuf: new");
    test_check(axl_ring_buf_get_capacity(rb) == 32, "ring_userbuf: cap 32");

    axl_ring_buf_push(rb, "hello", 5);
    char out[8];
    axl_ring_buf_pop(rb, out, 5);
    test_check(axl_memcmp(out, "hello", 5) == 0, "ring_userbuf: data");

    /* Free doesn't free caller's buffer */
    axl_ring_buf_free(rb);
    test_check(buf[0] == 'h', "ring_userbuf: buf intact after free");

    /* Non-power-of-2 rejected */
    test_check(axl_ring_buf_new_with_buffer(buf, 30, 0) == NULL,
               "ring_userbuf: reject non-pow2");
}

static void
test_ring_buf_push_pop_elem(void)
{
    AxlRingBuf *rb = axl_ring_buf_new_fixed(256, sizeof(int), 0);
    int vals[] = {10, 20, 30, 40, 50};

    /* Push 5 ints */
    for (int i = 0; i < 5; i++) {
        test_check(axl_ring_buf_push_elem(rb, &vals[i]) == AXL_OK,
                   "ring_welem: ok");
    }

    test_check(axl_ring_buf_get_length(rb) == 5,
               "ring_welem: count 5");

    /* Pop 5 ints — FIFO order */
    int out;
    for (int i = 0; i < 5; i++) {
        test_check(axl_ring_buf_pop_elem(rb, &out) == AXL_OK,
                   "ring_relem: ok");
        test_check(out == vals[i], "ring_relem: order");
    }

    /* Pop from empty fails */
    test_check(axl_ring_buf_pop_elem(rb, &out) == AXL_ERR,
               "ring_relem: empty fails");

    /* Push until full (reject mode) */
    AxlRingBuf *small = axl_ring_buf_new_fixed(16, sizeof(int), 0);
    int count = 0;
    int v = 99;
    while (axl_ring_buf_push_elem(small, &v) == AXL_OK) {
        count++;
    }
    test_check(count == 4, "ring_welem: 4 ints in 16 bytes");

    axl_ring_buf_free(small);
    axl_ring_buf_free(rb);
}

static void
test_ring_buf_peek_set_nth_elem(void)
{
    AxlRingBuf *rb = axl_ring_buf_new_fixed(256, sizeof(int), 0);
    int vals[] = {100, 200, 300, 400, 500};

    for (int i = 0; i < 5; i++) {
        axl_ring_buf_push_elem(rb, &vals[i]);
    }

    /* Get by index (0 = oldest) */
    int out;
    axl_ring_buf_peek_nth_elem(rb, 0, &out);
    test_check(out == 100, "ring_get: idx 0 = 100");

    axl_ring_buf_peek_nth_elem(rb, 4, &out);
    test_check(out == 500, "ring_get: idx 4 = 500");

    /* Out of range */
    test_check(axl_ring_buf_peek_nth_elem(rb, 5, &out) == AXL_ERR,
               "ring_get: out of range");

    /* Set by index */
    int newval = 999;
    test_check(axl_ring_buf_set_nth_elem(rb, 2, &newval) == AXL_OK,
               "ring_set: ok");
    axl_ring_buf_peek_nth_elem(rb, 2, &out);
    test_check(out == 999, "ring_set: modified");

    /* Other elements unchanged */
    axl_ring_buf_peek_nth_elem(rb, 1, &out);
    test_check(out == 200, "ring_set: neighbor intact");

    axl_ring_buf_free(rb);
}

static void
test_ring_buf_peek_elem(void)
{
    AxlRingBuf *rb = axl_ring_buf_new_fixed(64, sizeof(int), 0);
    int vals[] = {11, 22, 33};

    for (int i = 0; i < 3; i++) {
        axl_ring_buf_push_elem(rb, &vals[i]);
    }

    /* peek_elem gets head without consuming */
    int out = 0;
    test_check(axl_ring_buf_peek_elem(rb, &out) == AXL_OK, "ring_peek_elem: ok");
    test_check(out == 11, "ring_peek_elem: head value");

    /* readable unchanged */
    test_check(axl_ring_buf_get_length(rb) == 3,
               "ring_peek_elem: length unchanged");

    /* peek again gives same value */
    out = 0;
    axl_ring_buf_peek_elem(rb, &out);
    test_check(out == 11, "ring_peek_elem: still head");

    axl_ring_buf_free(rb);
}

static void
test_ring_buf_peek_msg(void)
{
    AxlRingBuf *rb = axl_ring_buf_new(256);

    /* Push two messages */
    axl_ring_buf_push_msg(rb, "first", 5);
    axl_ring_buf_push_msg(rb, "second", 6);

    /* peek_msg gets first without consuming */
    char buf[64];
    uint32_t actual;
    test_check(axl_ring_buf_peek_msg(rb, buf, sizeof(buf), &actual) == AXL_OK,
               "ring_peek_msg: ok");
    test_check(actual == 5, "ring_peek_msg: size 5");
    test_check(axl_memcmp(buf, "first", 5) == 0, "ring_peek_msg: data");

    /* peek again gives same message */
    test_check(axl_ring_buf_peek_msg(rb, buf, sizeof(buf), &actual) == AXL_OK,
               "ring_peek_msg: still first");
    test_check(actual == 5, "ring_peek_msg: size still 5");

    /* pop_msg consumes it */
    test_check(axl_ring_buf_pop_msg(rb, buf, sizeof(buf), &actual) == AXL_OK,
               "ring_peek_msg: pop ok");
    test_check(actual == 5, "ring_peek_msg: popped first");
    test_check(axl_memcmp(buf, "first", 5) == 0, "ring_peek_msg: popped data");

    /* Now peek gets second message */
    test_check(axl_ring_buf_peek_msg(rb, buf, sizeof(buf), &actual) == AXL_OK,
               "ring_peek_msg: next");
    test_check(actual == 6, "ring_peek_msg: size 6");
    test_check(axl_memcmp(buf, "second", 6) == 0, "ring_peek_msg: second data");

    axl_ring_buf_free(rb);
}

static void
test_ring_buf_messages(void)
{
    AxlRingBuf *rb = axl_ring_buf_new(256);

    /* Write 3 messages of different sizes */
    test_check(axl_ring_buf_push_msg(rb, "hi", 2) == AXL_OK, "ring_msg: write hi");
    test_check(axl_ring_buf_push_msg(rb, "hello world", 11) == AXL_OK,
               "ring_msg: write hello world");
    test_check(axl_ring_buf_push_msg(rb, "!", 1) == AXL_OK, "ring_msg: write !");

    /* Peek size of first message */
    test_check(axl_ring_buf_peek_msg_size(rb) == 2, "ring_msg: peek size 2");

    /* Read first message */
    char buf[64];
    uint32_t actual;
    test_check(axl_ring_buf_pop_msg(rb, buf, sizeof(buf), &actual) == AXL_OK,
               "ring_msg: read 1");
    test_check(actual == 2, "ring_msg: actual 2");
    test_check(axl_memcmp(buf, "hi", 2) == 0, "ring_msg: data hi");

    /* Read second */
    test_check(axl_ring_buf_pop_msg(rb, buf, sizeof(buf), &actual) == AXL_OK,
               "ring_msg: read 2");
    test_check(actual == 11, "ring_msg: actual 11");
    test_check(axl_memcmp(buf, "hello world", 11) == 0, "ring_msg: data hello");

    /* Read third */
    test_check(axl_ring_buf_pop_msg(rb, buf, sizeof(buf), &actual) == AXL_OK,
               "ring_msg: read 3");
    test_check(actual == 1, "ring_msg: actual 1");

    /* No more messages */
    test_check(axl_ring_buf_pop_msg(rb, buf, sizeof(buf), &actual) == AXL_ERR,
               "ring_msg: empty");
    test_check(axl_ring_buf_peek_msg_size(rb) == 0, "ring_msg: peek empty");

    /* Buffer too small for message */
    axl_ring_buf_push_msg(rb, "toolong", 7);
    test_check(axl_ring_buf_pop_msg(rb, buf, 3, &actual) == AXL_ERR,
               "ring_msg: dest too small");
    /* Message still there */
    test_check(axl_ring_buf_peek_msg_size(rb) == 7, "ring_msg: not consumed");

    /* NULL actual_len is ok */
    test_check(axl_ring_buf_pop_msg(rb, buf, sizeof(buf), NULL) == AXL_OK,
               "ring_msg: NULL actual_len");

    /* Not enough space in reject mode */
    AxlRingBuf *small = axl_ring_buf_new(16);
    test_check(axl_ring_buf_push_msg(small, buf, 20) == AXL_ERR,
               "ring_msg: reject too large");
    axl_ring_buf_free(small);

    axl_ring_buf_free(rb);
}

// ---------------------------------------------------------------------------
// Checksum Tests (GLib-style AxlChecksum API)
// ---------------------------------------------------------------------------

static void
test_checksum_sha1(void)
{
    /* One-shot convenience */
    char *hex = axl_compute_checksum(AXL_CHECKSUM_SHA1, "", 0);
    test_check(hex != NULL && axl_strcmp(hex, "da39a3ee5e6b4b0d3255bfef95601890afd80709") == 0,
               "checksum sha1: empty string");
    axl_free(hex);

    hex = axl_compute_checksum(AXL_CHECKSUM_SHA1, "abc", 3);
    test_check(hex != NULL && axl_strcmp(hex, "a9993e364706816aba3e25717850c26c9cd0d89d") == 0,
               "checksum sha1: abc");
    axl_free(hex);

    hex = axl_compute_checksum(AXL_CHECKSUM_SHA1,
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56);
    test_check(hex != NULL && axl_strcmp(hex, "84983e441c3bd26ebaae4aa1f95129e5e54670f1") == 0,
               "checksum sha1: 448-bit");
    axl_free(hex);
}

static void
test_checksum_md5(void)
{
    char *hex = axl_compute_checksum(AXL_CHECKSUM_MD5, "", 0);
    test_check(hex != NULL && axl_strcmp(hex, "d41d8cd98f00b204e9800998ecf8427e") == 0,
               "checksum md5: empty string");
    axl_free(hex);

    hex = axl_compute_checksum(AXL_CHECKSUM_MD5, "abc", 3);
    test_check(hex != NULL && axl_strcmp(hex, "900150983cd24fb0d6963f7d28e17f72") == 0,
               "checksum md5: abc");
    axl_free(hex);

    hex = axl_compute_checksum(AXL_CHECKSUM_MD5, "message digest", 14);
    test_check(hex != NULL && axl_strcmp(hex, "f96b697d7cb7938d525a2f31aaf161d0") == 0,
               "checksum md5: message digest");
    axl_free(hex);
}

static void
test_checksum_sha256(void)
{
    char *hex = axl_compute_checksum(AXL_CHECKSUM_SHA256, "", 0);
    test_check(hex != NULL && axl_strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0,
               "checksum sha256: empty string");
    axl_free(hex);

    hex = axl_compute_checksum(AXL_CHECKSUM_SHA256, "abc", 3);
    test_check(hex != NULL && axl_strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0,
               "checksum sha256: abc");
    axl_free(hex);
}

static void
test_checksum_incremental(void)
{
    /* Incremental SHA-1 should match one-shot */
    char *one_shot = axl_compute_checksum(AXL_CHECKSUM_SHA1, "hello world", 11);

    AxlChecksum *cs = axl_checksum_new(AXL_CHECKSUM_SHA1);
    test_check(cs != NULL, "checksum incremental: new");
    if (cs != NULL) {
        axl_checksum_update(cs, "hello", 5);
        axl_checksum_update(cs, " ", 1);
        axl_checksum_update(cs, "world", 5);
        const char *hex = axl_checksum_get_string(cs);
        test_check(hex != NULL && axl_strcmp(hex, one_shot) == 0,
                   "checksum incremental: sha1 matches one-shot");

        /* Reset and reuse */
        axl_checksum_reset(cs);
        axl_checksum_update(cs, "abc", 3);
        hex = axl_checksum_get_string(cs);
        test_check(hex != NULL && axl_strcmp(hex, "a9993e364706816aba3e25717850c26c9cd0d89d") == 0,
                   "checksum incremental: reset and reuse");

        axl_checksum_free(cs);
    }
    axl_free(one_shot);

    /* Incremental MD5 */
    one_shot = axl_compute_checksum(AXL_CHECKSUM_MD5, "hello world", 11);

    cs = axl_checksum_new(AXL_CHECKSUM_MD5);
    if (cs != NULL) {
        axl_checksum_update(cs, "hello world", 11);
        const char *hex = axl_checksum_get_string(cs);
        test_check(hex != NULL && axl_strcmp(hex, one_shot) == 0,
                   "checksum incremental: md5 matches one-shot");
        axl_checksum_free(cs);
    }
    axl_free(one_shot);
}

static void
test_checksum_get_digest(void)
{
    AxlChecksum *cs = axl_checksum_new(AXL_CHECKSUM_MD5);
    if (cs == NULL) {
        return;
    }

    axl_checksum_update(cs, "abc", 3);

    uint8_t digest[16];
    size_t dlen = sizeof(digest);
    axl_checksum_get_digest(cs, digest, &dlen);

    test_check(dlen == 16, "checksum get_digest: length 16");
    /* Verify first byte of MD5("abc") = 0x90 */
    test_check(digest[0] == 0x90, "checksum get_digest: first byte");

    axl_checksum_free(cs);
}

static void
test_crc32(void)
{
    /* Standard CRC-32 check value: "123456789" -> 0xCBF43926. */
    test_check(axl_crc32(0, "123456789", 9) == 0xCBF43926u,
               "crc32: check vector 123456789");

    /* Empty input with the zero seed is the identity. */
    test_check(axl_crc32(0, NULL, 0) == 0u,
               "crc32: empty input, seed 0");

    /* Well-known pangram vector. */
    const char *fox = "The quick brown fox jumps over the lazy dog";
    test_check(axl_crc32(0, fox, 43) == 0x414FA339u,
               "crc32: pangram");

    /* Chaining across calls equals the one-shot over the concatenation. */
    uint32_t crc = axl_crc32(0, "123", 3);
    crc = axl_crc32(crc, "456789", 6);
    test_check(crc == 0xCBF43926u,
               "crc32: incremental matches one-shot");

    /* A zero-length chunk mid-stream must not change the running value. */
    uint32_t a = axl_crc32(0, "12345", 5);
    uint32_t b = axl_crc32(a, NULL, 0);
    test_check(a == b, "crc32: zero-length chunk is a no-op");
}

static void
test_adler32(void)
{
    /* Adler-32 of the empty string with the seed (1) is 1. */
    test_check(axl_adler32(1, NULL, 0) == 1u,
               "adler32: empty input, seed 1");

    /* "Wikipedia" -> 0x11E60398 (the canonical RFC-1950 example). */
    test_check(axl_adler32(1, "Wikipedia", 9) == 0x11E60398u,
               "adler32: Wikipedia vector");

    /* "abc": s1 = 1+97+98+99 = 295 (0x127), s2 = 98+196+295 = 589 (0x24D). */
    test_check(axl_adler32(1, "abc", 3) == 0x024D0127u,
               "adler32: abc vector");

    /* Chaining equals the one-shot over the concatenation. */
    uint32_t adv = axl_adler32(1, "Wiki", 4);
    adv = axl_adler32(adv, "pedia", 5);
    test_check(adv == 0x11E60398u,
               "adler32: incremental matches one-shot");

    /* A zero-length chunk mid-stream is a no-op. */
    uint32_t x = axl_adler32(1, "hello", 5);
    uint32_t y = axl_adler32(x, NULL, 0);
    test_check(x == y, "adler32: zero-length chunk is a no-op");
}

// ---------------------------------------------------------------------------
// AxlCompress — one-shot codec (gzip / zlib / raw DEFLATE)
// ---------------------------------------------------------------------------

/* 264-byte plaintext compressed by the HOST python gzip/zlib below.
   Used for inbound interop (we must decode what real tools produce). */
static const char canned_plain[] =
    "AxlCompress interop: the quick brown fox jumps over the lazy"
    " dog. AxlCompress interop: the quick brown fox jumps over th"
    "e lazy dog. AxlCompress interop: the quick brown fox jumps o"
    "ver the lazy dog. AxlCompress interop: the quick brown fox j"
    "umps over the lazy dog. ";
#define CANNED_PLAIN_LEN 264u

/* gzip member with the FNAME flag set (the `gzip foo` CLI embeds the
   original filename) — proves the decoder skips optional header fields. */
static const uint8_t canned_gz_fname[] = {
    0x1f, 0x8b, 0x08, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x6f, 0x72,
    0x69, 0x67, 0x2e, 0x74, 0x78, 0x74, 0x00, 0x73, 0xac, 0xc8, 0x71, 0xce,
    0xcf, 0x2d, 0x28, 0x4a, 0x2d, 0x2e, 0x56, 0xc8, 0xcc, 0x2b, 0x49, 0x2d,
    0xca, 0x2f, 0xb0, 0x52, 0x28, 0xc9, 0x48, 0x55, 0x28, 0x2c, 0xcd, 0x4c,
    0xce, 0x56, 0x48, 0x2a, 0xca, 0x2f, 0xcf, 0x53, 0x48, 0xcb, 0xaf, 0x50,
    0xc8, 0x2a, 0xcd, 0x2d, 0x28, 0x56, 0xc8, 0x2f, 0x4b, 0x2d, 0x02, 0x4b,
    0xe7, 0x24, 0x56, 0x55, 0x2a, 0xa4, 0xe4, 0xa7, 0xeb, 0x29, 0x38, 0x0e,
    0x0b, 0x13, 0x00, 0xa9, 0x74, 0x4d, 0xf9, 0x08, 0x01, 0x00, 0x00,
};

/* zlib stream (level 9 → 0x78 0xda header) of the same plaintext. */
static const uint8_t canned_zlib[] = {
    0x78, 0xda, 0x73, 0xac, 0xc8, 0x71, 0xce, 0xcf, 0x2d, 0x28, 0x4a, 0x2d,
    0x2e, 0x56, 0xc8, 0xcc, 0x2b, 0x49, 0x2d, 0xca, 0x2f, 0xb0, 0x52, 0x28,
    0xc9, 0x48, 0x55, 0x28, 0x2c, 0xcd, 0x4c, 0xce, 0x56, 0x48, 0x2a, 0xca,
    0x2f, 0xcf, 0x53, 0x48, 0xcb, 0xaf, 0x50, 0xc8, 0x2a, 0xcd, 0x2d, 0x28,
    0x56, 0xc8, 0x2f, 0x4b, 0x2d, 0x02, 0x4b, 0xe7, 0x24, 0x56, 0x55, 0x2a,
    0xa4, 0xe4, 0xa7, 0xeb, 0x29, 0x38, 0x0e, 0x0b, 0x13, 0x00, 0x71, 0x1e,
    0x60, 0xcd,
};

/* Round-trip @p data through compress+decompress at @p fmt/@p level and
   assert the output is byte-identical. Returns the compressed size (for
   ratio checks) or (size_t)-1 on any failure. */
static size_t
roundtrip_compress(AxlCompressFormat fmt, int level,
                   const void *data, size_t len, const char *label)
{
    void  *comp = NULL;
    size_t comp_len = 0;
    if (axl_compress(fmt, data, len, &comp, &comp_len, level) != AXL_OK) {
        test_fail(label);
        return (size_t)-1;
    }
    void  *plain = NULL;
    size_t plain_len = 0;
    int rc = axl_decompress(fmt, comp, comp_len, &plain, &plain_len);
    bool ok = (rc == AXL_OK) && (plain_len == len)
              && (len == 0 || axl_memcmp(plain, data, len) == 0);
    test_check(ok, label);
    axl_free(comp);
    axl_free(plain);
    return ok ? comp_len : (size_t)-1;
}

static void
test_compress_roundtrip(void)
{
    const AxlCompressFormat fmts[] = {
        AXL_COMPRESS_GZIP, AXL_COMPRESS_ZLIB, AXL_COMPRESS_DEFLATE_RAW
    };
    const char *names[] = { "gzip", "zlib", "raw" };

    for (size_t i = 0; i < 3; i++) {
        char lbl[64];
        /* empty input */
        axl_snprintf(lbl, sizeof(lbl), "compress %s: empty round-trip", names[i]);
        roundtrip_compress(fmts[i], AXL_COMPRESS_LEVEL_DEFAULT, "", 0, lbl);

        /* short text */
        axl_snprintf(lbl, sizeof(lbl), "compress %s: short round-trip", names[i]);
        roundtrip_compress(fmts[i], AXL_COMPRESS_LEVEL_DEFAULT,
                           "hello, hello, hello", 19, lbl);

        /* highly compressible → must shrink (5000 identical bytes) */
        {
            char *blob = axl_malloc(5000);
            if (blob) {
                axl_memset(blob, 'Z', 5000);
                axl_snprintf(lbl, sizeof(lbl),
                             "compress %s: compressible shrinks", names[i]);
                size_t clen = roundtrip_compress(fmts[i],
                                                 AXL_COMPRESS_LEVEL_DEFAULT,
                                                 blob, 5000, lbl);
                axl_snprintf(lbl, sizeof(lbl),
                             "compress %s: ratio < 0.1 on 5000 Z", names[i]);
                test_check(clen != (size_t)-1 && clen < 500, lbl);
                axl_free(blob);
            }
        }

        /* larger pseudo-random buffer (LCG) crossing block boundaries */
        {
            size_t n = 100000;
            uint8_t *blob = axl_malloc(n);
            if (blob) {
                uint32_t s = 0x12345678u;
                for (size_t k = 0; k < n; k++) {
                    s = s * 1103515245u + 12345u;
                    blob[k] = (uint8_t)(s >> 16);
                }
                axl_snprintf(lbl, sizeof(lbl),
                             "compress %s: 100KB random round-trip", names[i]);
                roundtrip_compress(fmts[i], AXL_COMPRESS_LEVEL_DEFAULT,
                                   blob, n, lbl);
                axl_free(blob);
            }
        }
    }
}

/* LZMA-alone stream of the plaintext in test_lzma_decode_golden, generated by:
   python3: lzma.compress(pt, format=lzma.FORMAT_ALONE)
   then the 8-byte uncompressed-size field (bytes 5-12) patched to the
   real size (232) so the header is not a sentinel.  Verified that
   Python's lzma.decompress() round-trips the patched blob unchanged.
   Shared by test_lzma_decode_golden and test_lzma_negative. */
static const unsigned char lzma_golden[] = {
    0x5d, 0x00, 0x00, 0x80, 0x00, 0xe8, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x20, 0x9e, 0x09, 0x84, 0x65, 0xee, 0xce, 0xff,
    0xe5, 0x84, 0x46, 0xde, 0x31, 0x8d, 0x3f, 0x35, 0x7f, 0x00, 0x34,
    0x8d, 0xea, 0x57, 0x50, 0xb1, 0xd3, 0xd6, 0xe2, 0x45, 0x81, 0xb8,
    0x19, 0xeb, 0xab, 0x69, 0x49, 0x06, 0xf9, 0xa6, 0x5c, 0xa5, 0xb3,
    0x14, 0x06, 0x59, 0x11, 0x02, 0x03, 0x84, 0xcb, 0x3b, 0x7e, 0xc8,
    0x00, 0x4e, 0x51, 0x81, 0x40, 0xc5, 0x45, 0x90, 0xec, 0x9d, 0x51,
    0xff, 0xff, 0xfd, 0xa0, 0x90, 0x00 };
static const size_t lzma_golden_len = sizeof(lzma_golden);

static void
test_lzma_decode_golden(void)
{
    static const char expected[] =
        "AxlFw LZMA golden fixture: the quick brown fox 0123456789."
        "AxlFw LZMA golden fixture: the quick brown fox 0123456789."
        "AxlFw LZMA golden fixture: the quick brown fox 0123456789."
        "AxlFw LZMA golden fixture: the quick brown fox 0123456789.";
    void  *out = NULL;
    size_t out_len = 0;
    int rc = axl_decompress(AXL_COMPRESS_LZMA, lzma_golden, lzma_golden_len,
                            &out, &out_len);
    test_check(rc == AXL_OK, "lzma decode: returns AXL_OK");
    test_check(out_len == sizeof(expected) - 1, "lzma decode: exact length");
    test_check(out != NULL && axl_memcmp(out, expected, sizeof(expected) - 1) == 0,
               "lzma decode: byte-exact plaintext");
    axl_free(out);
}

static void
test_lzma_roundtrip(void)
{
    /* Empty input */
    {
        void  *c = NULL, *p = NULL;
        size_t cl = 0, pl = 0;
        int rc = axl_compress(AXL_COMPRESS_LZMA, "", 0, &c, &cl,
                              AXL_COMPRESS_LEVEL_DEFAULT);
        test_check(rc == AXL_OK, "lzma roundtrip: empty compress ok");
        if (rc == AXL_OK) {
            rc = axl_decompress(AXL_COMPRESS_LZMA, c, cl, &p, &pl);
            test_check(rc == AXL_OK && pl == 0,
                       "lzma roundtrip: empty decompress ok");
            axl_free(c);
            axl_free(p);
        }
    }

    /* Single byte */
    {
        void  *c = NULL, *p = NULL;
        size_t cl = 0, pl = 0;
        int rc = axl_compress(AXL_COMPRESS_LZMA, "a", 1, &c, &cl,
                              AXL_COMPRESS_LEVEL_DEFAULT);
        test_check(rc == AXL_OK, "lzma roundtrip: 1-byte compress ok");
        if (rc == AXL_OK) {
            rc = axl_decompress(AXL_COMPRESS_LZMA, c, cl, &p, &pl);
            test_check(rc == AXL_OK && pl == 1
                       && axl_memcmp(p, "a", 1) == 0,
                       "lzma roundtrip: 1-byte byte-exact");
            axl_free(c);
            axl_free(p);
        }
    }

    /* 64 KiB repeating pattern */
    {
        size_t len = 64u * 1024u;
        uint8_t *in = axl_malloc(len);
        if (in) {
            for (size_t i = 0; i < len; i++)
                in[i] = (uint8_t)(i & 0xFFu);
            void  *c = NULL, *p = NULL;
            size_t cl = 0, pl = 0;
            int rc = axl_compress(AXL_COMPRESS_LZMA, in, len, &c, &cl,
                                  AXL_COMPRESS_LEVEL_DEFAULT);
            test_check(rc == AXL_OK, "lzma roundtrip: 64KB compress ok");
            if (rc == AXL_OK) {
                rc = axl_decompress(AXL_COMPRESS_LZMA, c, cl, &p, &pl);
                test_check(rc == AXL_OK && pl == len
                           && axl_memcmp(p, in, len) == 0,
                           "lzma roundtrip: 64KB byte-exact");
                axl_free(c);
                axl_free(p);
            }
            axl_free(in);
        }
    }
}

static void
test_lzma_negative(void)
{
    /* Uses the file-scope lzma_golden[] fixture.  Mutating cases copy into a
       local buffer first; non-mutating cases pass lzma_golden directly. */
    void  *out;
    size_t out_len;

    /* Case 1: in_len < 13 (header too short — 5 bytes). */
    {
        out = (void *)1; out_len = 1;
        int rc = axl_decompress(AXL_COMPRESS_LZMA, lzma_golden, 5, &out, &out_len);
        test_check(rc == AXL_ERR,   "lzma negative: short header returns AXL_ERR");
        test_check(out == NULL,     "lzma negative: short header out==NULL");
        test_check(out_len == 0,    "lzma negative: short header out_len==0");
    }

    /* Case 2: invalid props byte (0xFF > 224). */
    {
        unsigned char buf[sizeof(lzma_golden)];
        axl_memcpy(buf, lzma_golden, lzma_golden_len);
        buf[0] = 0xFF;  /* props byte; valid range 0..224 */
        out = (void *)1; out_len = 1;
        int rc = axl_decompress(AXL_COMPRESS_LZMA, buf, lzma_golden_len, &out, &out_len);
        test_check(rc == AXL_ERR,   "lzma negative: bad props returns AXL_ERR");
        test_check(out == NULL,     "lzma negative: bad props out==NULL");
        test_check(out_len == 0,    "lzma negative: bad props out_len==0");
    }

    /* Case 3: forged uncompressed size > AXL_COMPRESS_MAX_OUTPUT (512 MiB).
       Use 0xF0000000 (~3.75 GiB), which is above 512 MiB but below the
       0xFFFFFFFFFFFFFFFF sentinel so it hits the cap check, not the sentinel path. */
    {
        unsigned char buf[sizeof(lzma_golden)];
        axl_memcpy(buf, lzma_golden, lzma_golden_len);
        /* bytes 5..12: little-endian uint64 = 0x00000000F0000000 */
        buf[5]  = 0x00; buf[6]  = 0x00; buf[7]  = 0x00; buf[8]  = 0xF0;
        buf[9]  = 0x00; buf[10] = 0x00; buf[11] = 0x00; buf[12] = 0x00;
        out = (void *)1; out_len = 1;
        int rc = axl_decompress(AXL_COMPRESS_LZMA, buf, lzma_golden_len, &out, &out_len);
        test_check(rc == AXL_ERR,   "lzma negative: over-cap size returns AXL_ERR");
        test_check(out == NULL,     "lzma negative: over-cap size out==NULL");
        test_check(out_len == 0,    "lzma negative: over-cap size out_len==0");
    }

    /* Case 4: truncated payload (header intact, body cut to 3 bytes after header).
       Rejected because the stream is incomplete (caught by the dest_len != unc /
       !finished checks in the decoder). */
    {
        /* Pass only 13 + 3 = 16 bytes; stream body is 3 bytes, far too short. */
        out = (void *)1; out_len = 1;
        int rc = axl_decompress(AXL_COMPRESS_LZMA, lzma_golden, 16, &out, &out_len);
        test_check(rc == AXL_ERR,   "lzma negative: truncated payload returns AXL_ERR");
        test_check(out == NULL,     "lzma negative: truncated payload out==NULL");
        test_check(out_len == 0,    "lzma negative: truncated payload out_len==0");
    }
}

static void
test_compress_levels(void)
{
    const char *data = "level test level test level test level test";
    roundtrip_compress(AXL_COMPRESS_GZIP, 0, data, 43, "compress: level 0 round-trip");
    roundtrip_compress(AXL_COMPRESS_GZIP, 9, data, 43, "compress: level 9 round-trip");
    /* out-of-range level is clamped, not rejected */
    roundtrip_compress(AXL_COMPRESS_GZIP, 99, data, 43, "compress: level 99 clamped round-trip");
}

static void
test_compress_gzip_framing(void)
{
    const char *data = "framing check framing check framing check";
    size_t len = 41;
    void  *gz = NULL;
    size_t gz_len = 0;
    if (axl_compress(AXL_COMPRESS_GZIP, data, len, &gz, &gz_len,
                     AXL_COMPRESS_LEVEL_DEFAULT) != AXL_OK) {
        test_fail("compress gzip framing: compress");
        return;
    }
    const uint8_t *b = gz;
    test_check(gz_len >= 18, "gzip framing: at least header+trailer");
    test_check(b[0] == 0x1f && b[1] == 0x8b, "gzip framing: magic 1f 8b");
    test_check(b[2] == 0x08, "gzip framing: CM = deflate (08)");

    /* ISIZE trailer (last 4 bytes, little-endian) == uncompressed len. */
    uint32_t isize = (uint32_t)b[gz_len - 4]
                   | ((uint32_t)b[gz_len - 3] << 8)
                   | ((uint32_t)b[gz_len - 2] << 16)
                   | ((uint32_t)b[gz_len - 1] << 24);
    test_check(isize == len, "gzip framing: ISIZE == input length");

    /* CRC-32 trailer (4 bytes before ISIZE, little-endian) == crc of input. */
    uint32_t crc = (uint32_t)b[gz_len - 8]
                 | ((uint32_t)b[gz_len - 7] << 8)
                 | ((uint32_t)b[gz_len - 6] << 16)
                 | ((uint32_t)b[gz_len - 5] << 24);
    test_check(crc == axl_crc32(0, data, len), "gzip framing: CRC-32 matches");
    axl_free(gz);
}

static void
test_compress_interop_inbound(void)
{
    /* Decode a real `gzip`-produced member (FNAME flag set). */
    void  *out = NULL;
    size_t out_len = 0;
    int rc = axl_decompress(AXL_COMPRESS_GZIP, canned_gz_fname,
                            sizeof(canned_gz_fname), &out, &out_len);
    test_check(rc == AXL_OK && out_len == CANNED_PLAIN_LEN
               && axl_memcmp(out, canned_plain, CANNED_PLAIN_LEN) == 0,
               "compress interop: decode host gzip (skips FNAME)");
    axl_free(out);

    /* Decode a real zlib stream (0x78 0xda). */
    out = NULL; out_len = 0;
    rc = axl_decompress(AXL_COMPRESS_ZLIB, canned_zlib,
                        sizeof(canned_zlib), &out, &out_len);
    test_check(rc == AXL_OK && out_len == CANNED_PLAIN_LEN
               && axl_memcmp(out, canned_plain, CANNED_PLAIN_LEN) == 0,
               "compress interop: decode host zlib");
    axl_free(out);
}

static void
test_compress_errors(void)
{
    void  *out = NULL;
    size_t out_len = 0;

    /* Wrong magic for gzip. */
    uint8_t bad_magic[20] = { 0x00, 0x00, 0x08 };
    test_check(axl_decompress(AXL_COMPRESS_GZIP, bad_magic, sizeof(bad_magic),
                              &out, &out_len) == AXL_ERR,
               "compress error: gzip wrong magic rejected");

    /* Truncated gzip (header only, no trailer). */
    test_check(axl_decompress(AXL_COMPRESS_GZIP, canned_gz_fname, 12,
                              &out, &out_len) == AXL_ERR,
               "compress error: truncated gzip rejected");

    /* Corrupt payload → CRC mismatch. Copy the canned member, flip a
       body byte (well past the 19-byte header, before the 8-byte trailer). */
    uint8_t corrupt[sizeof(canned_gz_fname)];
    axl_memcpy(corrupt, canned_gz_fname, sizeof(canned_gz_fname));
    corrupt[30] ^= 0xFF;
    test_check(axl_decompress(AXL_COMPRESS_GZIP, corrupt, sizeof(corrupt),
                              &out, &out_len) == AXL_ERR,
               "compress error: corrupt gzip body caught (CRC/inflate)");

    /* NULL output pointer. */
    test_check(axl_compress(AXL_COMPRESS_GZIP, "x", 1, NULL, &out_len,
                            AXL_COMPRESS_LEVEL_DEFAULT) == AXL_ERR,
               "compress error: NULL out pointer rejected");

    /* Forged gzip ISIZE claiming ~4 GiB → must be rejected by the
       output cap before any giant allocation, not honored. */
    uint8_t big_isize[sizeof(canned_gz_fname)];
    axl_memcpy(big_isize, canned_gz_fname, sizeof(canned_gz_fname));
    big_isize[sizeof(big_isize) - 4] = 0xFF;
    big_isize[sizeof(big_isize) - 3] = 0xFF;
    big_isize[sizeof(big_isize) - 2] = 0xFF;
    big_isize[sizeof(big_isize) - 1] = 0xFF;
    test_check(axl_decompress(AXL_COMPRESS_GZIP, big_isize, sizeof(big_isize),
                              &out, &out_len) == AXL_ERR,
               "compress error: forged 4 GiB ISIZE rejected (output cap)");

    /* zlib with a corrupt body → Adler-32 mismatch. */
    uint8_t zbad[sizeof(canned_zlib)];
    axl_memcpy(zbad, canned_zlib, sizeof(canned_zlib));
    zbad[20] ^= 0xFF;  /* flip a body byte (past the 2-byte header) */
    test_check(axl_decompress(AXL_COMPRESS_ZLIB, zbad, sizeof(zbad),
                              &out, &out_len) == AXL_ERR,
               "compress error: corrupt zlib body caught (Adler/inflate)");

    /* zlib with an invalid header (fails the %31 check). */
    uint8_t zhdr[8] = { 0x78, 0x00, 0, 0, 0, 0, 0, 0 };  /* 0x7800 % 31 != 0 */
    test_check(axl_decompress(AXL_COMPRESS_ZLIB, zhdr, sizeof(zhdr),
                              &out, &out_len) == AXL_ERR,
               "compress error: zlib bad header (%31) rejected");
}

// ---------------------------------------------------------------------------
// AxlCompress — stream filters
// ---------------------------------------------------------------------------

/* Read an entire stream into a freshly allocated buffer. */
static uint8_t *
read_all_stream(AxlStream *s, size_t *out_len)
{
    AxlStream *acc = axl_bufopen();
    if (acc == NULL) {
        return NULL;
    }
    uint8_t tmp[1024];
    for (;;) {
        axl_ssize_t got = axl_read(s, tmp, sizeof(tmp));
        if (got <= 0) {
            break;
        }
        axl_write(acc, tmp, (size_t)got);
    }
    size_t      n   = 0;
    const void *dat = axl_bufdata(acc, &n);
    uint8_t    *buf = axl_malloc(n == 0 ? 1 : n);
    if (buf != NULL && n != 0) {
        axl_memcpy(buf, dat, n);
    }
    *out_len = n;
    axl_fclose(acc);
    return buf;
}

static void
test_compress_writer(void)
{
    const char *data = "stream writer payload stream writer payload stream";
    size_t len = 50;

    /* Explicit finish: write through a gzip writer into a buffer sink,
       then decode the sink and compare. */
    AxlStream *sink = axl_bufopen();
    AxlStream *w    = axl_gzip_writer(sink, AXL_COMPRESS_LEVEL_DEFAULT);
    test_check(w != NULL, "compress writer: created");
    if (w != NULL) {
        axl_write(w, data, len);
        int rc = axl_compress_writer_finish(w);
        test_check(rc == AXL_OK, "compress writer: finish ok");

        size_t      gzn = 0;
        const void *gz  = axl_bufdata(sink, &gzn);
        void       *plain = NULL;
        size_t      pn = 0;
        int drc = axl_decompress(AXL_COMPRESS_GZIP, gz, gzn, &plain, &pn);
        test_check(drc == AXL_OK && pn == len
                   && axl_memcmp(plain, data, len) == 0,
                   "compress writer: sink holds valid gzip of input");
        axl_free(plain);

        /* finish is idempotent. */
        test_check(axl_compress_writer_finish(w) == AXL_OK,
                   "compress writer: finish idempotent");
        axl_fclose(w);
    }
    axl_fclose(sink);

    /* finish on a non-writer stream is rejected. */
    AxlStream *plainbuf = axl_bufopen();
    test_check(axl_compress_writer_finish(plainbuf) == AXL_ERR,
               "compress writer: finish on non-writer rejected");
    axl_fclose(plainbuf);

    /* Implicit finalize on close: no explicit finish, fclose must flush
       a valid stream to the sink. Sink is closed after the writer. */
    AxlStream *sink2 = axl_bufopen();
    AxlStream *w2    = axl_gzip_writer(sink2, AXL_COMPRESS_LEVEL_DEFAULT);
    if (w2 != NULL) {
        axl_write(w2, data, len);
        axl_fclose(w2);  /* implicit finish */
        size_t      gzn = 0;
        const void *gz  = axl_bufdata(sink2, &gzn);
        void       *plain = NULL;
        size_t      pn = 0;
        int drc = axl_decompress(AXL_COMPRESS_GZIP, gz, gzn, &plain, &pn);
        test_check(drc == AXL_OK && pn == len
                   && axl_memcmp(plain, data, len) == 0,
                   "compress writer: fclose finalizes implicitly");
        axl_free(plain);
    }
    axl_fclose(sink2);
}

static void
test_compress_reader(void)
{
    const char *data = "stream reader payload, repeated. stream reader payload.";
    size_t len = 55;

    /* End-to-end through the stream layer: gzip writer -> buffer ->
       gzip reader -> compare. */
    AxlStream *mid = axl_bufopen();
    AxlStream *w   = axl_gzip_writer(mid, AXL_COMPRESS_LEVEL_DEFAULT);
    axl_write(w, data, len);
    axl_compress_writer_finish(w);
    axl_fclose(w);
    axl_fseek(mid, 0, AXL_SEEK_SET);

    AxlStream *r = axl_gzip_reader(mid);
    test_check(r != NULL, "compress reader: created over gzip stream");
    if (r != NULL) {
        size_t   pn = 0;
        uint8_t *plain = read_all_stream(r, &pn);
        test_check(pn == len && plain != NULL
                   && axl_memcmp(plain, data, len) == 0,
                   "compress reader: round-trips writer output");
        axl_free(plain);

        /* Seekable: rewind and re-read the first few bytes. */
        axl_fseek(r, 0, AXL_SEEK_SET);
        char head[6] = {0};
        axl_read(r, head, 5);
        test_check(axl_memcmp(head, data, 5) == 0,
                   "compress reader: seek SET re-reads plaintext");

        /* End position equals the plaintext length. */
        axl_fseek(r, 0, AXL_SEEK_END);
        test_check(axl_ftell(r) == (int64_t)len,
                   "compress reader: SEEK_END tell == plaintext length");
        axl_fclose(r);
    }
    axl_fclose(mid);

    /* Generic format param (zlib) also works. */
    void  *zl = NULL;
    size_t zln = 0;
    axl_compress(AXL_COMPRESS_ZLIB, data, len, &zl, &zln,
                 AXL_COMPRESS_LEVEL_DEFAULT);
    AxlStream *zsrc = axl_bufopen();
    axl_write(zsrc, zl, zln);
    axl_fseek(zsrc, 0, AXL_SEEK_SET);
    axl_free(zl);
    AxlStream *zr = axl_compress_reader(AXL_COMPRESS_ZLIB, zsrc);
    test_check(zr != NULL, "compress reader: zlib format param");
    if (zr != NULL) {
        size_t   pn = 0;
        uint8_t *plain = read_all_stream(zr, &pn);
        test_check(pn == len && plain != NULL
                   && axl_memcmp(plain, data, len) == 0,
                   "compress reader: zlib round-trips");
        axl_free(plain);
        axl_fclose(zr);
    }
    axl_fclose(zsrc);

    /* A corrupt stream yields NULL at construction. */
    AxlStream *bsrc = axl_bufopen();
    uint8_t junk[32];
    axl_memset(junk, 0x5A, sizeof(junk));
    axl_write(bsrc, junk, sizeof(junk));
    axl_fseek(bsrc, 0, AXL_SEEK_SET);
    test_check(axl_gzip_reader(bsrc) == NULL,
               "compress reader: corrupt gzip source yields NULL");
    axl_fclose(bsrc);
}

static void
test_checksum_type_length(void)
{
    test_check(axl_checksum_type_get_length(AXL_CHECKSUM_MD5) == 16,
               "checksum type_length: md5 = 16");
    test_check(axl_checksum_type_get_length(AXL_CHECKSUM_SHA1) == 20,
               "checksum type_length: sha1 = 20");
    test_check(axl_checksum_type_get_length(AXL_CHECKSUM_SHA256) == 32,
               "checksum type_length: sha256 = 32");
    test_check(axl_checksum_type_get_length((AxlChecksumType)99) == 0,
               "checksum type_length: unknown = 0");
}

// ---------------------------------------------------------------------------
// OOM fault injection — exercise error paths in container constructors
// and mutators. Each block sets axl_mem_fail_next_alloc() and verifies
// the constructor returns NULL / the mutator returns -1 cleanly.
// ---------------------------------------------------------------------------

static void
test_oom_containers(void)
{
    /* --- Hash table --- */

    /* Constructor first alloc fails */
    axl_mem_fail_next_alloc(1);
    AxlHashTable *h = axl_hash_table_new_str();
    test_check(h == NULL, "oom: axl_hash_table_new_str returns NULL on first-alloc fail");

    /* Insert returning -1 on OOM: build a real table, then fail the
       next alloc — the first distinct insert needs to allocate a
       new node. The hash table insert API returns tri-state int:
       1 new, 0 replaced, -1 OOM (or NULL input). */
    h = axl_hash_table_new_str();
    test_check(h != NULL, "oom: hash table reconstructed cleanly after failure");
    axl_mem_fail_next_alloc(1);
    int rc = axl_hash_table_insert(h, "key", (void *)1);
    test_check(rc == -1, "oom: hash_table_insert returns -1 on node alloc fail");
    test_check(axl_hash_table_size(h) == 0, "oom: no entry stored on insert failure");
    axl_hash_table_free(h);

    /* --- Radix tree --- */

    axl_mem_fail_next_alloc(1);
    AxlRadixTree *tr = axl_radix_tree_new();
    test_check(tr == NULL, "oom: axl_radix_tree_new returns NULL on first-alloc fail");

    /* Insert into an existing tree — returns -1 on alloc failure */
    tr = axl_radix_tree_new();
    test_check(tr != NULL, "oom: radix tree reconstructed cleanly");
    axl_mem_fail_next_alloc(1);
    int r2 = axl_radix_tree_insert(tr, "alpha", (void *)1);
    test_check(r2 == AXL_ERR, "oom: axl_radix_tree_insert returns -1 on node alloc fail");
    axl_radix_tree_free(tr);

    /* --- N-ary tree --- */

    axl_mem_fail_next_alloc(1);
    AxlNTree *nt = axl_ntree_new((void *)1);
    test_check(nt == NULL, "oom: axl_ntree_new returns NULL on first-alloc fail");

    nt = axl_ntree_new((void *)1);
    test_check(nt != NULL, "oom: ntree reconstructed cleanly after failure");
    axl_mem_fail_next_alloc(1);
    AxlNTree *kid = axl_ntree_append_data(nt, (void *)2);
    test_check(kid == NULL, "oom: axl_ntree_append_data returns NULL on node alloc fail");
    test_check(axl_ntree_n_children(nt) == 0, "oom: no child attached on alloc failure");
    axl_ntree_free(nt);

    /* --- AVL tree --- */

    axl_mem_fail_next_alloc(1);
    AxlTree *avt = axl_tree_new(tree_cmp_intptr, NULL);
    test_check(avt == NULL, "oom: axl_tree_new returns NULL on first-alloc fail");

    avt = axl_tree_new(tree_cmp_intptr, NULL);
    test_check(avt != NULL, "oom: tree reconstructed cleanly after failure");
    axl_mem_fail_next_alloc(1);
    axl_tree_insert(avt, (void *)1, (void *)2);
    test_check(axl_tree_nnodes(avt) == 0,
               "oom: tree insert leaves tree unchanged on node alloc fail");
    axl_tree_free(avt);

    /* --- Ring buffer --- */

    axl_mem_fail_next_alloc(1);
    AxlRingBuf *rb = axl_ring_buf_new(64);
    test_check(rb == NULL, "oom: axl_ring_buf_new returns NULL on first-alloc fail");

    /* Header allocates, then buffer allocates — fail the 2nd to cover
       the header-cleanup path (constructor must free the already-made
       header). */
    axl_mem_fail_next_alloc(2);
    rb = axl_ring_buf_new(64);
    test_check(rb == NULL, "oom: axl_ring_buf_new returns NULL on storage alloc fail");

    /* --- Dynamic array --- */

    axl_mem_fail_next_alloc(1);
    AxlArray *arr = axl_array_new(sizeof(int));
    test_check(arr == NULL, "oom: axl_array_new returns NULL on first-alloc fail");

    /* Grow failure: create a small array, fill it, then fail the
       next alloc — append should return without growing. */
    arr = axl_array_new(sizeof(int));
    test_check(arr != NULL, "oom: array reconstructed cleanly");
    for (int i = 0; i < 8; i++) {
        int val = i;
        axl_array_append(arr, &val);
    }
    size_t before = axl_array_len(arr);
    /* Force many appends until the underlying buffer grows; the grow
       call will allocate, so inject the next failure. */
    axl_mem_fail_next_alloc(1);
    /* Append enough values that at least one grow is required. */
    for (int i = 0; i < 1024; i++) {
        int val = 100 + i;
        if (axl_array_append(arr, &val) != AXL_OK) {
            break;
        }
    }
    test_check(axl_array_len(arr) >= before,
               "oom: array_append grow failure leaves existing data intact");
    axl_array_free(arr);

    /* --- String duplicate via axl_strdup — already covered in
       test_oom_allocator_primitives, but the hash table keys also
       strdup internally, so covering it here documents the intent. */
    axl_mem_fail_next_alloc(1);
    char *s = axl_strdup("test");
    test_check(s == NULL, "oom: axl_strdup returns NULL on injected failure");

    /* Clear the counter in case something above left it set. */
    axl_mem_fail_next_alloc(0);
}

// ---------------------------------------------------------------------------
// GLib-parity additions: hash-table int/int64/double hash+equal,
// add/remove_all/find/get_keys/get_values; array insert/prepend.
// ---------------------------------------------------------------------------

static void
test_hash_typed_funcs(void)
{
    // int_equal / int64_equal / double_equal direct checks.
    int ia = 7, ib = 7, ic = 9;
    test_check(axl_int_equal(&ia, &ib), "int_equal: 7 == 7");
    test_check(!axl_int_equal(&ia, &ic), "int_equal: 7 != 9");
    test_check(axl_int_hash(&ia) == axl_int_hash(&ib), "int_hash: equal keys, equal hash");

    int64_t la = 0x1122334455667788LL, lb = 0x1122334455667788LL, lc = -1;
    test_check(axl_int64_equal(&la, &lb), "int64_equal: equal");
    test_check(!axl_int64_equal(&la, &lc), "int64_equal: differ");
    test_check(axl_int64_hash(&la) == axl_int64_hash(&lb), "int64_hash: equal keys, equal hash");

    double da = 3.5, db = 3.5, dz = 0.0, dnz = -0.0;
    test_check(axl_double_equal(&da, &db), "double_equal: 3.5 == 3.5");
    test_check(axl_double_equal(&dz, &dnz), "double_equal: 0.0 == -0.0");
    test_check(axl_double_hash(&dz) == axl_double_hash(&dnz),
        "double_hash: -0.0 hashes as +0.0");

    // int-keyed table round-trip.
    AxlHashTable *t = axl_hash_table_new(axl_int_hash, axl_int_equal);
    test_check(t != NULL, "int table: new");
    int k1 = 100, k2 = 200, miss = 300;
    axl_hash_table_insert(t, &k1, (void *)0xAA);
    axl_hash_table_insert(t, &k2, (void *)0xBB);
    test_check(axl_hash_table_lookup(t, &k1) == (void *)0xAA, "int table: lookup k1");
    test_check(axl_hash_table_lookup(t, &k2) == (void *)0xBB, "int table: lookup k2");
    test_check(axl_hash_table_lookup(t, &miss) == NULL, "int table: lookup missing");
    axl_hash_table_free(t);
}

static void
test_hash_add(void)
{
    AxlHashTable *t = axl_hash_table_new(axl_str_hash, axl_str_equal);
    char a[] = "alpha";
    char a2[] = "alpha";  // equal-but-distinct pointer
    test_check(axl_hash_table_add(t, a), "add: new key -> true");
    test_check(!axl_hash_table_add(t, a2), "add: existing key -> false");
    test_check(axl_hash_table_contains(t, "alpha"), "add: key present");
    test_check(axl_hash_table_size(t) == 1, "add: size 1 after dup add");
    axl_hash_table_free(t);
}

static void
test_hash_remove_all(void)
{
    AxlHashTable *t = axl_hash_table_new_str();
    axl_hash_table_insert(t, "a", (void *)1);
    axl_hash_table_insert(t, "b", (void *)2);
    axl_hash_table_insert(t, "c", (void *)3);
    test_check(axl_hash_table_size(t) == 3, "remove_all: 3 before");
    axl_hash_table_remove_all(t);
    test_check(axl_hash_table_size(t) == 0, "remove_all: 0 after");
    test_check(!axl_hash_table_contains(t, "a"), "remove_all: 'a' gone");
    // Reusable after clear.
    axl_hash_table_insert(t, "d", (void *)4);
    test_check(axl_hash_table_lookup(t, "d") == (void *)4, "remove_all: reusable");
    axl_hash_table_remove_all(NULL);  // NULL-safe
    axl_hash_table_free(t);
}

static bool
find_value_is(const void *key, void *value, void *data)
{
    (void)key;
    return value == data;
}

static void
test_hash_find(void)
{
    AxlHashTable *t = axl_hash_table_new_str();
    axl_hash_table_insert(t, "x", (void *)10);
    axl_hash_table_insert(t, "y", (void *)20);
    axl_hash_table_insert(t, "z", (void *)30);
    test_check(axl_hash_table_find(t, find_value_is, (void *)20) == (void *)20,
        "find: matching value returned");
    test_check(axl_hash_table_find(t, find_value_is, (void *)999) == NULL,
        "find: no match -> NULL");
    test_check(axl_hash_table_find(NULL, find_value_is, NULL) == NULL,
        "find: NULL table -> NULL");
    axl_hash_table_free(t);
}

static void
test_hash_get_keys_values(void)
{
    AxlHashTable *t = axl_hash_table_new_str();
    axl_hash_table_insert(t, "a", (void *)1);
    axl_hash_table_insert(t, "b", (void *)2);
    axl_hash_table_insert(t, "c", (void *)3);

    AxlList *keys = axl_hash_table_get_keys(t);
    test_check(axl_list_length(keys) == 3, "get_keys: length 3");
    // Order unspecified — check each expected key is present, value sum via list.
    bool saw_a = false, saw_b = false, saw_c = false;
    for (AxlList *l = keys; l != NULL; l = l->next) {
        if (axl_strcmp((const char *)l->data, "a") == 0) { saw_a = true; }
        if (axl_strcmp((const char *)l->data, "b") == 0) { saw_b = true; }
        if (axl_strcmp((const char *)l->data, "c") == 0) { saw_c = true; }
    }
    test_check(saw_a && saw_b && saw_c, "get_keys: all keys present");
    axl_list_free(keys);  // spine only; keys belong to the table

    AxlList *values = axl_hash_table_get_values(t);
    test_check(axl_list_length(values) == 3, "get_values: length 3");
    size_t sum = 0;
    for (AxlList *l = values; l != NULL; l = l->next) {
        sum += (size_t)l->data;
    }
    test_check(sum == 6, "get_values: 1+2+3 == 6");
    axl_list_free(values);

    test_check(axl_hash_table_get_keys(NULL) == NULL, "get_keys: NULL table -> NULL");
    axl_hash_table_free(t);
}

static void
test_array_insert_prepend(void)
{
    // Value mode: insert at end, front, middle; verify order.
    AxlArray *a = axl_array_new(sizeof(int));
    int v;
    v = 1; axl_array_append(a, &v);   // [1]
    v = 3; axl_array_append(a, &v);   // [1,3]
    v = 2; test_check(axl_array_insert(a, 1, &v) == AXL_OK, "insert: middle ok");  // [1,2,3]
    v = 0; test_check(axl_array_prepend(a, &v) == AXL_OK, "prepend: front ok");    // [0,1,2,3]
    v = 4; test_check(axl_array_insert(a, axl_array_len(a), &v) == AXL_OK,
        "insert: at length == append");                                           // [0,1,2,3,4]

    test_check(axl_array_len(a) == 5, "insert: length 5");
    bool ordered = true;
    for (int i = 0; i < 5; i++) {
        if (*(int *)axl_array_get(a, (size_t)i) != i) { ordered = false; }
    }
    test_check(ordered, "insert/prepend: [0,1,2,3,4] in order");

    // Out-of-range insert rejected, array unchanged.
    v = 9;
    test_check(axl_array_insert(a, 99, &v) == AXL_ERR, "insert: index>len -> ERR");
    test_check(axl_array_len(a) == 5, "insert: length unchanged after ERR");
    axl_array_free(a);

    // Pointer mode.
    AxlArray *p = axl_array_new(sizeof(void *));
    axl_array_append_ptr(p, (void *)0xB);                 // [B]
    test_check(axl_array_prepend_ptr(p, (void *)0xA) == AXL_OK, "prepend_ptr: ok");  // [A,B]
    test_check(axl_array_insert_ptr(p, 2, (void *)0xC) == AXL_OK, "insert_ptr: end"); // [A,B,C]
    test_check(axl_array_get_ptr(p, 0) == (void *)0xA, "ptr: [0]=A");
    test_check(axl_array_get_ptr(p, 1) == (void *)0xB, "ptr: [1]=B");
    test_check(axl_array_get_ptr(p, 2) == (void *)0xC, "ptr: [2]=C");
    axl_array_free(p);

    // Drive insert past the initial capacity so the realloc-during-shift
    // path runs: prepend 40 ints, expect [39,38,...,0] reversed -> ascending
    // read 0..39 from the tail. (INITIAL_CAPACITY is well under 40.)
    AxlArray *g = axl_array_new(sizeof(int));
    bool prepend_ok = true;
    for (int i = 0; i < 40; i++) {
        int x = i;
        if (axl_array_prepend(g, &x) != AXL_OK) { prepend_ok = false; }
    }
    test_check(prepend_ok, "insert-grow: 40 prepends across realloc succeed");
    test_check(axl_array_len(g) == 40, "insert-grow: length 40");
    bool grow_ok = true;
    for (int i = 0; i < 40; i++) {
        // element 0 is the last prepended (39); element i is 39-i.
        if (*(int *)axl_array_get(g, (size_t)i) != 39 - i) { grow_ok = false; }
    }
    test_check(grow_ok, "insert-grow: order intact across realloc");
    axl_array_free(g);
}

static void
test_hash_add_owned_set(void)
{
    // Canonical owned-key set: new_full with a key destructor, no value
    // destructor. add() must not double-free, and the table owns the keys.
    AxlHashTable *t = axl_hash_table_new_full(
        axl_str_hash, axl_str_equal, axl_free_impl, NULL);
    test_check(axl_hash_table_add(t, axl_strdup("one")), "owned-set: add 'one' new");
    test_check(axl_hash_table_add(t, axl_strdup("two")), "owned-set: add 'two' new");
    // Duplicate key: add replaces (keeping the new strdup, freeing the old).
    test_check(!axl_hash_table_add(t, axl_strdup("one")), "owned-set: dup 'one' -> false");
    test_check(axl_hash_table_size(t) == 2, "owned-set: size 2");
    test_check(axl_hash_table_contains(t, "two"), "owned-set: contains 'two'");
    axl_hash_table_free(t);  // frees the owned keys; no double-free
}

// ---------------------------------------------------------------------------
// AxlHmac (RFC 2104) — expected values are the canonical RFC test vectors,
// independent of this implementation.
// ---------------------------------------------------------------------------

static void
test_hmac_rfc_vectors(void)
{
    // key="Jefe", msg="what do ya want for nothing?" — RFC 2202 / 4231.
    const char *jefe = "Jefe";
    const char *msg  = "what do ya want for nothing?";
    size_t      kl   = 4;
    size_t      ml   = 28;

    char *md5 = axl_compute_hmac(AXL_CHECKSUM_MD5, jefe, kl, msg, ml);
    test_check(md5 != NULL && axl_strcmp(md5, "750c783e6ab0b503eaa86e310a5db738") == 0,
        "hmac: MD5 Jefe vector");
    axl_free(md5);

    char *sha1 = axl_compute_hmac(AXL_CHECKSUM_SHA1, jefe, kl, msg, ml);
    test_check(sha1 != NULL &&
        axl_strcmp(sha1, "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79") == 0,
        "hmac: SHA1 Jefe vector");
    axl_free(sha1);

    char *sha256 = axl_compute_hmac(AXL_CHECKSUM_SHA256, jefe, kl, msg, ml);
    test_check(sha256 != NULL && axl_strcmp(sha256,
        "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843") == 0,
        "hmac: SHA256 Jefe vector");
    axl_free(sha256);
}

static void
test_hmac_edge_cases(void)
{
    // Key longer than the 64-byte block -> hashed down first.
    uint8_t longkey[80];
    axl_memset(longkey, 0xAA, sizeof(longkey));
    uint8_t msg50[50];
    axl_memset(msg50, 'x', sizeof(msg50));
    char *lk = axl_compute_hmac(AXL_CHECKSUM_SHA256, longkey, sizeof(longkey),
                                msg50, sizeof(msg50));
    test_check(lk != NULL && axl_strcmp(lk,
        "f38c5a07bcad2aa4f0b3d7a00fc2cd3779ef03b366fbab5230c19958fac1eb5f") == 0,
        "hmac: SHA256 long-key (hashed) vector");
    axl_free(lk);

    // Exactly block-sized key (boundary, no hashing).
    uint8_t key64[64];
    axl_memset(key64, 0xBB, sizeof(key64));
    char *k64 = axl_compute_hmac(AXL_CHECKSUM_SHA256, key64, sizeof(key64), "hi", 2);
    test_check(k64 != NULL && axl_strcmp(k64,
        "8d44220390018e84fdc833b80030721efe5eba614de5e1a52c42084fb54a925f") == 0,
        "hmac: SHA256 64-byte key boundary");
    axl_free(k64);

    // Empty key, empty message.
    char *e = axl_compute_hmac(AXL_CHECKSUM_SHA256, NULL, 0, "", 0);
    test_check(e != NULL && axl_strcmp(e,
        "b613679a0814d9ec772f95d778c35fc5ff1697c493715653c6c712144292c5ad") == 0,
        "hmac: SHA256 empty key + empty message");
    axl_free(e);

    // Unsupported type / invalid args.
    test_check(axl_hmac_new((AxlChecksumType)999, "k", 1) == NULL,
        "hmac: unsupported type -> NULL");
    test_check(axl_hmac_new(AXL_CHECKSUM_SHA256, NULL, 5) == NULL,
        "hmac: NULL key with len>0 -> NULL");
}

static void
test_hmac_incremental(void)
{
    // Incremental update must equal one-shot over the same bytes.
    AXL_AUTOPTR(AxlHmac) h = axl_hmac_new(AXL_CHECKSUM_SHA256, "key", 3);
    axl_hmac_update(h, "what do ya ", 11);
    axl_hmac_update(h, "want for ", 9);
    axl_hmac_update(h, "nothing?", 8);
    const char *inc = axl_hmac_get_string(h);

    char *oneshot = axl_compute_hmac(AXL_CHECKSUM_SHA256, "key", 3,
                                     "what do ya want for nothing?", 28);
    test_check(oneshot != NULL && axl_strcmp(inc, oneshot) == 0,
        "hmac: incremental == one-shot");

    // get_string is idempotent (repeated calls, same pointer/value).
    test_check(axl_hmac_get_string(h) == inc, "hmac: get_string idempotent pointer");

    // Raw digest path matches the hex string (32 bytes for SHA-256).
    uint8_t raw[32];
    size_t  rlen = sizeof(raw);
    axl_hmac_get_digest(h, raw, &rlen);
    test_check(rlen == 32, "hmac: SHA256 digest length 32");
    char hex[65];
    static const char HX[] = "0123456789abcdef";
    for (size_t i = 0; i < rlen; i++) {
        hex[2 * i]     = HX[raw[i] >> 4];
        hex[2 * i + 1] = HX[raw[i] & 0x0F];
    }
    hex[2 * rlen] = '\0';
    test_check(axl_strcmp(hex, inc) == 0, "hmac: get_digest matches get_string");

    axl_free(oneshot);

    // MD5 get_digest exercises the 16-byte digest-length path.
    AXL_AUTOPTR(AxlHmac) hm = axl_hmac_new(AXL_CHECKSUM_MD5, "Jefe", 4);
    axl_hmac_update(hm, "what do ya want for nothing?", 28);
    uint8_t md5raw[16];
    size_t  mlen = sizeof(md5raw);
    axl_hmac_get_digest(hm, md5raw, &mlen);
    test_check(mlen == 16, "hmac: MD5 digest length 16");
    // First two bytes of 750c... are 0x75, 0x0c.
    test_check(md5raw[0] == 0x75 && md5raw[1] == 0x0c, "hmac: MD5 digest bytes");

    // Truncating get_digest: small buffer gets *len bytes, *len reports full.
    AXL_AUTOPTR(AxlHmac) ht = axl_hmac_new(AXL_CHECKSUM_SHA256, "Jefe", 4);
    axl_hmac_update(ht, "what do ya want for nothing?", 28);
    uint8_t small[10];
    axl_memset(small, 0xEE, sizeof(small));
    size_t slen = 10;
    axl_hmac_get_digest(ht, small, &slen);
    test_check(slen == 32, "hmac: truncated get_digest reports full length 32");
    // First 10 bytes of 5bdcc146bf60754e6a04... = 5b dc c1 46 bf 60 75 4e 6a 04.
    test_check(small[0] == 0x5b && small[9] == 0x04, "hmac: truncated get_digest first 10 bytes");
}

// ---------------------------------------------------------------------------
// AxlBytes — immutable refcounted byte buffer (GBytes analog).
// ---------------------------------------------------------------------------

static void
test_bytes_basic(void)
{
    char src[] = {1, 2, 3, 4, 5};
    AxlBytes *b = axl_bytes_new(src, sizeof(src));
    test_check(b != NULL, "bytes: new -> non-NULL");
    test_check(axl_bytes_get_size(b) == 5, "bytes: size 5");

    // new() copies — mutating the source must not change the buffer.
    src[0] = 99;
    size_t n;
    const uint8_t *p = axl_bytes_get_data(b, &n);
    test_check(n == 5, "bytes: get_data size 5");
    test_check(p[0] == 1 && p[4] == 5, "bytes: copied, source mutation isolated");
    axl_bytes_unref(b);

    // Empty buffer.
    AxlBytes *e = axl_bytes_new(NULL, 0);
    test_check(e != NULL && axl_bytes_get_size(e) == 0, "bytes: empty new ok");
    test_check(axl_bytes_get_data(e, &n) == NULL && n == 0, "bytes: empty data NULL");
    axl_bytes_unref(e);

    // Invalid: NULL data with non-zero size.
    test_check(axl_bytes_new(NULL, 5) == NULL, "bytes: new(NULL,5) -> NULL");
    test_check(axl_bytes_new_take(NULL, 5) == NULL, "bytes: new_take(NULL,5) -> NULL");

    // NULL-safety.
    test_check(axl_bytes_get_size(NULL) == 0, "bytes: get_size(NULL) 0");
    test_check(axl_bytes_get_data(NULL, &n) == NULL && n == 0, "bytes: get_data(NULL) NULL");
    axl_bytes_unref(NULL);
}

static void
test_bytes_storage_flavors(void)
{
    // static: borrows, no copy — the data pointer is the literal.
    static const char lit[] = "static-blob";
    AxlBytes *s = axl_bytes_new_static(lit, sizeof(lit) - 1);
    test_check(axl_bytes_get_data(s, NULL) == (const void *)lit,
        "bytes: new_static borrows (no copy)");
    axl_bytes_unref(s);

    // take: owns the heap block, no copy — same pointer.
    char *heap = axl_malloc(4);
    heap[0] = 'A'; heap[1] = 'B'; heap[2] = 'C'; heap[3] = 'D';
    AxlBytes *t = axl_bytes_new_take(heap, 4);
    test_check(axl_bytes_get_data(t, NULL) == heap, "bytes: new_take owns (no copy)");
    axl_bytes_unref(t);  // frees heap

    // new_take(NULL, 0): valid empty buffer; unref must not free garbage.
    AxlBytes *te = axl_bytes_new_take(NULL, 0);
    test_check(te != NULL && axl_bytes_get_size(te) == 0, "bytes: new_take empty ok");
    axl_bytes_unref(te);

    // new_take(heap, 0): owns a heap block but is empty — normalized to the
    // empty shape (get_data NULL), and the block is freed (no leak under
    // AXL_MEM_DEBUG).
    char *empty_heap = axl_malloc(8);
    AxlBytes *th = axl_bytes_new_take(empty_heap, 0);
    test_check(th != NULL && axl_bytes_get_data(th, NULL) == NULL,
        "bytes: new_take(heap,0) -> empty shape, get_data NULL");
    axl_bytes_unref(th);
}

static void
test_bytes_refcount(void)
{
    char src[] = {9, 8, 7};
    AxlBytes *b = axl_bytes_new(src, 3);
    AxlBytes *b2 = axl_bytes_ref(b);
    test_check(b2 == b, "bytes: ref returns same object");
    axl_bytes_unref(b);  // refcount 2 -> 1, still alive
    const uint8_t *p = axl_bytes_get_data(b2, NULL);
    test_check(p[0] == 9 && p[2] == 7, "bytes: alive after one unref of two refs");
    axl_bytes_unref(b2);  // -> 0, freed
    test_check(axl_bytes_ref(NULL) == NULL, "bytes: ref(NULL) -> NULL");
}

static void
test_bytes_slice(void)
{
    char src[] = {10, 11, 12, 13, 14, 15};
    AxlBytes *parent = axl_bytes_new(src, 6);
    const uint8_t *pp = axl_bytes_get_data(parent, NULL);

    // Zero-copy slice: data points into the parent's storage.
    AxlBytes *mid = axl_bytes_new_from_bytes(parent, 2, 3);  // {12,13,14}
    test_check(axl_bytes_get_size(mid) == 3, "bytes: slice size 3");
    const uint8_t *mp = axl_bytes_get_data(mid, NULL);
    test_check(mp == pp + 2, "bytes: slice shares parent storage (zero-copy)");
    test_check(mp[0] == 12 && mp[2] == 14, "bytes: slice content");

    // Slice keeps the parent alive after the parent ref is dropped.
    axl_bytes_unref(parent);
    test_check(mp[0] == 12 && mp[2] == 14, "bytes: parent kept alive by slice");
    axl_bytes_unref(mid);

    // Bounds checks.
    AxlBytes *p2 = axl_bytes_new(src, 6);
    test_check(axl_bytes_new_from_bytes(p2, 4, 3) == NULL, "bytes: slice past end -> NULL");
    test_check(axl_bytes_new_from_bytes(p2, 7, 0) == NULL, "bytes: slice offset>size -> NULL");
    test_check(axl_bytes_new_from_bytes(NULL, 0, 0) == NULL, "bytes: slice(NULL) -> NULL");

    // Whole-range slice returns a reference to the parent itself.
    AxlBytes *whole = axl_bytes_new_from_bytes(p2, 0, 6);
    test_check(whole == p2, "bytes: whole-range slice -> ref to parent");
    axl_bytes_unref(whole);  // drop the extra ref

    // Slice-of-a-slice: the parent chain is a linked list, each link
    // holding one ref. Drop the intermediate; the grandchild keeps the
    // whole chain alive.
    AxlBytes *s1 = axl_bytes_new_from_bytes(p2, 1, 4);   // {11,12,13,14}
    AxlBytes *s2 = axl_bytes_new_from_bytes(s1, 1, 2);   // {12,13}
    const uint8_t *s2p = axl_bytes_get_data(s2, NULL);
    test_check(axl_bytes_get_size(s2) == 2 && s2p[0] == 12 && s2p[1] == 13,
        "bytes: slice-of-slice content");
    axl_bytes_unref(s1);   // intermediate dropped; s2 holds it alive
    axl_bytes_unref(p2);   // root dropped; chain still alive via s2
    test_check(s2p[0] == 12 && s2p[1] == 13, "bytes: slice-of-slice keeps chain alive");
    axl_bytes_unref(s2);   // releases s1 -> p2 in turn
}

static void
test_bytes_compare(void)
{
    AxlBytes *a  = axl_bytes_new("abc", 3);
    AxlBytes *a2 = axl_bytes_new("abc", 3);   // equal content, distinct object
    AxlBytes *b  = axl_bytes_new("abd", 3);
    AxlBytes *ab = axl_bytes_new("ab", 2);    // prefix of "abc"

    test_check(axl_bytes_equal(a, a2), "bytes: equal same content");
    test_check(!axl_bytes_equal(a, b), "bytes: not equal diff content");
    test_check(!axl_bytes_equal(a, ab), "bytes: not equal diff size");
    test_check(axl_bytes_hash(a) == axl_bytes_hash(a2), "bytes: equal content -> equal hash");

    test_check(axl_bytes_compare(a, a2) == 0, "bytes: compare equal -> 0");
    test_check(axl_bytes_compare(a, b) < 0, "bytes: 'abc' < 'abd'");
    test_check(axl_bytes_compare(b, a) > 0, "bytes: 'abd' > 'abc'");
    test_check(axl_bytes_compare(ab, a) < 0, "bytes: prefix 'ab' < 'abc'");

    // Empty-buffer comparisons (size==0 edge paths).
    AxlBytes *e1 = axl_bytes_new(NULL, 0);
    AxlBytes *e2 = axl_bytes_new(NULL, 0);
    test_check(axl_bytes_equal(e1, e2), "bytes: empty == empty");
    test_check(axl_bytes_hash(e1) == axl_bytes_hash(e2), "bytes: empty hashes equal");
    test_check(axl_bytes_compare(e1, e2) == 0, "bytes: empty compare 0");
    test_check(axl_bytes_compare(e1, a) < 0, "bytes: empty < 'abc'");
    test_check(axl_bytes_equal(NULL, NULL), "bytes: equal(NULL,NULL) true");
    test_check(!axl_bytes_equal(a, NULL), "bytes: equal(a,NULL) false");

    axl_bytes_unref(e1);
    axl_bytes_unref(e2);
    axl_bytes_unref(a);
    axl_bytes_unref(a2);
    axl_bytes_unref(b);
    axl_bytes_unref(ab);
}

// ---------------------------------------------------------------------------
// Entry Point
// ---------------------------------------------------------------------------

int
test_data_main(int argc, char **argv)
{
    (void)argc; (void)argv;
    test_print_header("AxlData");

    test_hash();
    test_hash_new_full();
    test_hash_contains();
    test_hash_steal();
    test_hash_foreach_remove();
    test_hash_iter();
    test_hash_direct();
    test_hash_insert_vs_replace();
    test_hash_steal_copy_keys();
    test_hash_typed_funcs();
    test_hash_add();
    test_hash_add_owned_set();
    test_hash_remove_all();
    test_hash_find();
    test_hash_get_keys_values();
    test_array();
    test_array_extended();
    test_array_insert_prepend();
    test_hmac_rfc_vectors();
    test_hmac_edge_cases();
    test_hmac_incremental();
    test_bytes_basic();
    test_bytes_storage_flavors();
    test_bytes_refcount();
    test_bytes_slice();
    test_bytes_compare();
    test_string();
    test_string_ascii();
    test_json_parse();
    test_json_get_object();
    test_json5_parse();
    test_json_load_file();
    test_json_build();
    test_json5_build();
    test_json_print();
    test_slist();
    test_list();
    test_queue();
    test_list_extended();
    test_slist_extended();
    test_queue_extended();
    test_str_split();
    test_str_join();
    test_str_strip();
    test_str_search();
    test_str_test();
    test_json_escape();
    test_cache();
    test_page_cache();
    test_page_cache_multitenant();
    test_rb_tree();
    test_text_buffer();
    test_text_buffer_nav();
    test_radix_tree();
    test_radix_tree_prefix();
    test_radix_tree_edge_split();
    test_radix_tree_foreach();
    test_radix_tree_value_free();
    test_radix_tree_http_keys();
    test_ntree_build_navigate();
    test_ntree_query_traverse();
    test_ntree_unlink_free();
    test_ntree_iter();
    test_ntree_iter_reverse();
    test_ntree_move();
    test_tree_basic();
    test_tree_balance();
    test_tree_remove();
    test_tree_insert_replace_destroy();
    test_tree_bounds();
    test_tree_iter();
    test_ring_buf();
    test_ring_buf_wrap();
    test_ring_buf_overwrite();
    test_ring_buf_peek_discard();
    test_ring_buf_regions();
    test_ring_buf_partial();
    test_ring_buf_push_stats();
    test_ring_buf_power_of_2();
    test_ring_buf_init();
    test_ring_buf_user_buffer();
    test_ring_buf_push_pop_elem();
    test_ring_buf_peek_set_nth_elem();
    test_ring_buf_peek_elem();
    test_ring_buf_peek_msg();
    test_ring_buf_messages();

    axl_printf("\n--- Checksum ---\n");
    test_checksum_sha1();
    test_checksum_md5();
    test_checksum_sha256();
    test_checksum_incremental();
    test_checksum_get_digest();
    test_crc32();
    test_adler32();
    test_compress_roundtrip();
    test_lzma_decode_golden();
    test_lzma_roundtrip();
    test_lzma_negative();
    test_compress_levels();
    test_compress_gzip_framing();
    test_compress_interop_inbound();
    test_compress_errors();
    test_compress_writer();
    test_compress_reader();
    test_checksum_type_length();

    axl_printf("\n--- OOM Injection ---\n");
    test_oom_containers();

    return test_print_results();
}

AXL_APP(test_data_main)
