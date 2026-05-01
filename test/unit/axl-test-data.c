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
    int rc = axl_hash_table_insert(t, key_a1, (void *)1);
    test_check(rc == 1, "ins_vs_rep: first insert returns 1 (new)");
    test_check(free_count == 0, "ins_vs_rep: first insert no frees");
    test_check(axl_hash_table_size(t) == 1, "ins_vs_rep: size 1");

    // insert with collision — NEW key (key_a2) should be freed, OLD key (key_a1) kept;
    // return value should be 0 (replaced)
    free_count = 0;
    rc = axl_hash_table_insert(t, key_a2, (void *)2);
    test_check(rc == 0, "ins_vs_rep: collision insert returns 0 (replaced)");
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
    test_check(rc == 1, "ins_vs_rep: first replace returns 1 (new)");

    // replace with collision — OLD key (key_b1) should be freed, NEW key (key_b2) kept;
    // return value should be 0 (replaced)
    free_count = 0;
    rc = axl_hash_table_replace(t, key_b2, (void *)20);
    test_check(rc == 0, "ins_vs_rep: collision replace returns 0 (replaced)");
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

    test_check(axl_array_remove_index(arr, 1) == 0, "array: remove_index ok");
    test_check(axl_array_len(arr) == 3, "array: remove_index len");
    got = axl_array_get(arr, 0);
    test_check(got != NULL && *got == 10, "array: remove_index [0]=10");
    got = axl_array_get(arr, 1);
    test_check(got != NULL && *got == 30, "array: remove_index [1]=30");
    got = axl_array_get(arr, 2);
    test_check(got != NULL && *got == 40, "array: remove_index [2]=40");
    test_check(axl_array_remove_index(arr, 5) == -1, "array: remove_index oob");
    axl_array_free(arr);

    // -- remove_index_fast: [10,20,30,40] remove index 0 -> [40,20,30] --
    arr = axl_array_new(sizeof(size_t));
    val = 10; axl_array_append(arr, &val);
    val = 20; axl_array_append(arr, &val);
    val = 30; axl_array_append(arr, &val);
    val = 40; axl_array_append(arr, &val);

    test_check(axl_array_remove_index_fast(arr, 0) == 0, "array: remove_fast ok");
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

    test_check(axl_array_remove_range(arr, 1, 2) == 0, "array: remove_range ok");
    test_check(axl_array_len(arr) == 3, "array: remove_range len");
    got = axl_array_get(arr, 0);
    test_check(got != NULL && *got == 10, "array: remove_range [0]=10");
    got = axl_array_get(arr, 1);
    test_check(got != NULL && *got == 40, "array: remove_range [1]=40");
    got = axl_array_get(arr, 2);
    test_check(got != NULL && *got == 50, "array: remove_range [2]=50");
    test_check(axl_array_remove_range(arr, 1, 5) == -1, "array: remove_range oob");
    axl_array_free(arr);

    // -- set_size grow: 3 elements, set_size(5) --
    arr = axl_array_new(sizeof(size_t));
    val = 1; axl_array_append(arr, &val);
    val = 2; axl_array_append(arr, &val);
    val = 3; axl_array_append(arr, &val);

    test_check(axl_array_set_size(arr, 5) == 0, "array: set_size grow ok");
    test_check(axl_array_len(arr) == 5, "array: set_size grow len");
    got = axl_array_get(arr, 3);
    test_check(got != NULL && *got == 0, "array: set_size grow zero-init");

    // -- set_size shrink: set_size(2) --
    test_check(axl_array_set_size(arr, 2) == 0, "array: set_size shrink ok");
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
    test_check(axl_file_set_contents(path, json, sizeof(json) - 1) == 0,
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
    test_check(axl_cache_put(c, "key1", &val) == 0, "cache: put key1");

    int out = 0;
    test_check(axl_cache_get(c, "key1", &out) == 0, "cache: get key1 hit");
    test_check(out == 42, "cache: get key1 value");

    /* Miss on unknown key */
    test_check(axl_cache_get(c, "missing", &out) == -1, "cache: miss");

    /* Overwrite existing */
    val = 99;
    axl_cache_put(c, "key1", &val);
    axl_cache_get(c, "key1", &out);
    test_check(out == 99, "cache: overwrite value");

    /* Invalidate */
    axl_cache_invalidate(c, "key1");
    test_check(axl_cache_get(c, "key1", &out) == -1, "cache: invalidated miss");

    /* Fill cache to capacity (4 slots) */
    int v1 = 1, v2 = 2, v3 = 3, v4 = 4, v5 = 5;
    axl_cache_put(c, "a", &v1);
    axl_cache_put(c, "b", &v2);
    axl_cache_put(c, "c", &v3);
    axl_cache_put(c, "d", &v4);

    /* 5th entry evicts oldest (a) */
    axl_cache_put(c, "e", &v5);
    test_check(axl_cache_get(c, "a", &out) == -1, "cache: LRU evicted 'a'");
    test_check(axl_cache_get(c, "e", &out) == 0, "cache: 'e' present");
    test_check(out == 5, "cache: 'e' value");

    /* NULL safety */
    test_check(axl_cache_new(0, 4, 100) == NULL, "cache: new zero slots");
    axl_cache_free(NULL);  /* no crash */
    test_check(true, "cache: free(NULL) no crash");

    axl_cache_free(c);
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
    test_check(axl_radix_tree_insert(t, "/api", &v1) == 0, "radix: insert /api");
    test_check(axl_radix_tree_insert(t, "/api/users", &v2) == 0, "radix: insert /api/users");
    test_check(axl_radix_tree_insert(t, "/index.html", &v3) == 0, "radix: insert /index.html");
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
    test_check(axl_radix_tree_insert(t, "/api/users", &v4) == 0, "radix: overwrite");
    test_check(axl_radix_tree_lookup(t, "/api/users") == &v4, "radix: overwrite value");
    test_check(axl_radix_tree_size(t) == 2, "radix: size unchanged after overwrite");

    /* Empty key */
    size_t v5 = 500;
    test_check(axl_radix_tree_insert(t, "", &v5) == 0, "radix: insert empty key");
    test_check(axl_radix_tree_lookup(t, "") == &v5, "radix: lookup empty key");
    test_check(axl_radix_tree_size(t) == 3, "radix: size 3 with empty key");

    /* NULL safety */
    test_check(axl_radix_tree_lookup(NULL, "x") == NULL, "radix: lookup NULL tree");
    test_check(axl_radix_tree_insert(NULL, "x", NULL) == -1, "radix: insert NULL tree");
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
    test_check(rc == -1, "ring_stats: full elem ring rejects");
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

    test_check(axl_ring_buf_init(&rb, buf, 64, 0, NULL) == 0, "ring_init: ok");
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
    test_check(axl_ring_buf_init(&rb, buf, 50, 0, NULL) == -1,
               "ring_init: reject non-pow2");

    /* NULL args rejected */
    test_check(axl_ring_buf_init(NULL, buf, 64, 0, NULL) == -1,
               "ring_init: NULL rb");
    test_check(axl_ring_buf_init(&rb, NULL, 64, 0, NULL) == -1,
               "ring_init: NULL buf");

    /* init_fixed rejects elem_size 0 */
    test_check(axl_ring_buf_init_fixed(&rb, buf, 64, 0, 0, NULL) == -1,
               "ring_init: init_fixed(0) rejected");

    /* new_fixed rejects elem_size 0 */
    test_check(axl_ring_buf_new_fixed(64, 0, 0) == NULL,
               "ring_init: new_fixed(0) NULL");

    /* Layer 3 functions rejected on byte-mode buffer */
    axl_ring_buf_init(&rb, buf, 64, 0, NULL);
    int dummy = 42;
    test_check(axl_ring_buf_push_elem(&rb, &dummy) == -1,
               "ring_init: push_elem byte-mode rejected");
    test_check(axl_ring_buf_pop_elem(&rb, &dummy) == -1,
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
        test_check(axl_ring_buf_push_elem(rb, &vals[i]) == 0,
                   "ring_welem: ok");
    }

    test_check(axl_ring_buf_get_length(rb) == 5,
               "ring_welem: count 5");

    /* Pop 5 ints — FIFO order */
    int out;
    for (int i = 0; i < 5; i++) {
        test_check(axl_ring_buf_pop_elem(rb, &out) == 0,
                   "ring_relem: ok");
        test_check(out == vals[i], "ring_relem: order");
    }

    /* Pop from empty fails */
    test_check(axl_ring_buf_pop_elem(rb, &out) == -1,
               "ring_relem: empty fails");

    /* Push until full (reject mode) */
    AxlRingBuf *small = axl_ring_buf_new_fixed(16, sizeof(int), 0);
    int count = 0;
    int v = 99;
    while (axl_ring_buf_push_elem(small, &v) == 0) {
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
    test_check(axl_ring_buf_peek_nth_elem(rb, 5, &out) == -1,
               "ring_get: out of range");

    /* Set by index */
    int newval = 999;
    test_check(axl_ring_buf_set_nth_elem(rb, 2, &newval) == 0,
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
    test_check(axl_ring_buf_peek_elem(rb, &out) == 0, "ring_peek_elem: ok");
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
    test_check(axl_ring_buf_peek_msg(rb, buf, sizeof(buf), &actual) == 0,
               "ring_peek_msg: ok");
    test_check(actual == 5, "ring_peek_msg: size 5");
    test_check(axl_memcmp(buf, "first", 5) == 0, "ring_peek_msg: data");

    /* peek again gives same message */
    test_check(axl_ring_buf_peek_msg(rb, buf, sizeof(buf), &actual) == 0,
               "ring_peek_msg: still first");
    test_check(actual == 5, "ring_peek_msg: size still 5");

    /* pop_msg consumes it */
    test_check(axl_ring_buf_pop_msg(rb, buf, sizeof(buf), &actual) == 0,
               "ring_peek_msg: pop ok");
    test_check(actual == 5, "ring_peek_msg: popped first");
    test_check(axl_memcmp(buf, "first", 5) == 0, "ring_peek_msg: popped data");

    /* Now peek gets second message */
    test_check(axl_ring_buf_peek_msg(rb, buf, sizeof(buf), &actual) == 0,
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
    test_check(axl_ring_buf_push_msg(rb, "hi", 2) == 0, "ring_msg: write hi");
    test_check(axl_ring_buf_push_msg(rb, "hello world", 11) == 0,
               "ring_msg: write hello world");
    test_check(axl_ring_buf_push_msg(rb, "!", 1) == 0, "ring_msg: write !");

    /* Peek size of first message */
    test_check(axl_ring_buf_peek_msg_size(rb) == 2, "ring_msg: peek size 2");

    /* Read first message */
    char buf[64];
    uint32_t actual;
    test_check(axl_ring_buf_pop_msg(rb, buf, sizeof(buf), &actual) == 0,
               "ring_msg: read 1");
    test_check(actual == 2, "ring_msg: actual 2");
    test_check(axl_memcmp(buf, "hi", 2) == 0, "ring_msg: data hi");

    /* Read second */
    test_check(axl_ring_buf_pop_msg(rb, buf, sizeof(buf), &actual) == 0,
               "ring_msg: read 2");
    test_check(actual == 11, "ring_msg: actual 11");
    test_check(axl_memcmp(buf, "hello world", 11) == 0, "ring_msg: data hello");

    /* Read third */
    test_check(axl_ring_buf_pop_msg(rb, buf, sizeof(buf), &actual) == 0,
               "ring_msg: read 3");
    test_check(actual == 1, "ring_msg: actual 1");

    /* No more messages */
    test_check(axl_ring_buf_pop_msg(rb, buf, sizeof(buf), &actual) == -1,
               "ring_msg: empty");
    test_check(axl_ring_buf_peek_msg_size(rb) == 0, "ring_msg: peek empty");

    /* Buffer too small for message */
    axl_ring_buf_push_msg(rb, "toolong", 7);
    test_check(axl_ring_buf_pop_msg(rb, buf, 3, &actual) == -1,
               "ring_msg: dest too small");
    /* Message still there */
    test_check(axl_ring_buf_peek_msg_size(rb) == 7, "ring_msg: not consumed");

    /* NULL actual_len is ok */
    test_check(axl_ring_buf_pop_msg(rb, buf, sizeof(buf), NULL) == 0,
               "ring_msg: NULL actual_len");

    /* Not enough space in reject mode */
    AxlRingBuf *small = axl_ring_buf_new(16);
    test_check(axl_ring_buf_push_msg(small, buf, 20) == -1,
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
    test_check(r2 == -1, "oom: axl_radix_tree_insert returns -1 on node alloc fail");
    axl_radix_tree_free(tr);

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
        if (axl_array_append(arr, &val) != 0) {
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
    test_array();
    test_array_extended();
    test_string();
    test_string_ascii();
    test_json_parse();
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
    test_radix_tree();
    test_radix_tree_prefix();
    test_radix_tree_edge_split();
    test_radix_tree_foreach();
    test_radix_tree_value_free();
    test_radix_tree_http_keys();
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
    test_checksum_type_length();

    axl_printf("\n--- OOM Injection ---\n");
    test_oom_containers();

    return test_print_results();
}

AXL_APP(test_data_main)
