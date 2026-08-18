/** @file axl-test-data.c
    Test application for AxlData — hash, array, string, JSON.
**/

#include "axl-test.h"

#include <stdint.h>

// ---------------------------------------------------------------------------
// Hash Table Tests
// ---------------------------------------------------------------------------

/* Proves AXL_JSON_INDENT stays a CONSTANT EXPRESSION: a file-scope
   initializer will not compile against a function-call form, and nothing
   else in the suite would catch the regression -- C99 lets automatic
   aggregates take non-constant initializers, so every in-function use
   compiles either way. */
static const AxlJsonFlags kIndentConstExpr =
    AXL_JSON_INDENT(2) | AXL_JSON_COMPACT;

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

    /* Enough entries to force SEVERAL resizes (64 buckets, grow at 75%), and
       EVERY key is checked rather than the first and last.
       Checking two of them cannot see a bucket-index defect: the index is
       derived from the hash, so a wrong derivation scatters SOME keys and
       leaves others where they were. The property this pins is that INSERT
       and LOOKUP agree on the index across a resize — which is what actually
       breaks if one of the seven index sites in axl-hash-table.c is changed
       and the others are not. (It does NOT pin bucket_count's power-of-two
       property: that governs distribution, not correctness, and no test can
       see it. A static assert guards it at the source instead.) */
    t = axl_hash_table_new_str();
    for (size_t i = 0; i < 1024; i++) {
        char key_buf[16];
        axl_snprintf(key_buf, sizeof(key_buf), "key%04u", (unsigned)i);
        axl_hash_table_insert(t, key_buf, (void *)(size_t)(i + 1));
    }
    test_check(axl_hash_table_size(t) == 1024,
               "hash: 1024 entries after bulk insert");

    size_t found = 0;
    for (size_t i = 0; i < 1024; i++) {
        char key_buf[16];
        axl_snprintf(key_buf, sizeof(key_buf), "key%04u", (unsigned)i);
        if (axl_hash_table_lookup(t, key_buf) == (void *)(size_t)(i + 1)) {
            found++;
        }
    }
    test_check(found == 1024,
               "hash: EVERY key round-trips across several resizes");

    /* The negative half. Without it a table that returned a non-NULL value for
       anything would pass the positive check by construction. */
    size_t absent = 0;
    for (size_t i = 0; i < 1024; i++) {
        char key_buf[16];
        axl_snprintf(key_buf, sizeof(key_buf), "nope%04u", (unsigned)i);
        if (axl_hash_table_lookup(t, key_buf) == NULL) {
            absent++;
        }
    }
    test_check(absent == 1024,
               "hash: and 1024 absent keys all miss, so the hit count above "
               "is not vacuous");

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

    ok = axl_json_parse(json, axl_strlen(json), AXL_JSON_RELAXED, &r);
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
    ok = axl_json_parse("not json", 8, AXL_JSON_RELAXED, &r);
    test_check(!ok, "json parse: invalid returns false");
}

// The decoded-length queries. Every assertion is stated against what the
// matching accessor actually WRITES, not against a literal: the contract is
// "the buffer size that cannot truncate", so an agreement test is what pins
// it. A literal would also silently encode the escape arithmetic twice.
static void
test_json_decoded_len(void)
{
    // Each of these decodes to a DIFFERENT length than its source span, and in
    // both directions: \uXXXX shrinks 6 bytes to 1-3, a surrogate pair shrinks
    // 12 to 4, and JSON5's \0 GROWS 2 to the 3 of U+FFFD. A length derived
    // from the source span would pass for the plain case and fail for these.
    const char *doc =
        "{\"plain\":\"hello\","
        " \"esc\":\"a\\u0041b\","          // 3 chars decoded
        " \"wide\":\"\\u00e9\","            // 1 char, 2 UTF-8 bytes
        " \"pair\":\"\\ud83d\\ude00\","     // 1 code point, 4 UTF-8 bytes
        " \"empty\":\"\","
        " \"num\":42,"
        " \"arr\":[\"one\",\"a\\u0041b\"]}";
    AxlJsonReader r;
    test_check(axl_json_parse(doc, axl_strlen(doc), AXL_JSON_STRICT, &r),
        "jsonlen: fixture parses");

    size_t n = 0;

    // The core agreement: n + 1 is exactly enough, and what lands is what the
    // accessor writes with an oversized buffer.
    struct { const char *key; const char *want; } cases[] = {
        { "plain", "hello" },
        { "esc",   "aAb"   },
        { "wide",  "\xc3\xa9" },
        { "pair",  "\xf0\x9f\x98\x80" },
        { "empty", ""      },
    };
    bool all_agree = true;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        size_t len = 0;
        if (!axl_json_get_string_len(&r, cases[i].key, &len)) {
            all_agree = false;
            continue;
        }
        char exact[64];
        axl_json_get_string(&r, cases[i].key, exact, len + 1);
        if (axl_strlen(exact) != len || axl_strcmp(exact, cases[i].want) != 0) {
            all_agree = false;
        }
    }
    test_check(all_agree,
        "jsonlen: len+1 holds the whole value, for 5 escape shapes");

    // Named individually so a regression says WHICH shape broke.
    axl_json_get_string_len(&r, "esc", &n);
    test_check(n == 3, "jsonlen: \\u0041 counts decoded (3), not source (7)");
    axl_json_get_string_len(&r, "wide", &n);
    test_check(n == 2, "jsonlen: \\u00e9 is 2 UTF-8 bytes");
    axl_json_get_string_len(&r, "pair", &n);
    test_check(n == 4, "jsonlen: a surrogate pair is 4 UTF-8 bytes");
    n = 999;
    axl_json_get_string_len(&r, "empty", &n);
    test_check(n == 0, "jsonlen: an empty string is 0");

    // Refusals leave out_len untouched, matching axl_json_get_string's
    // untouched-on-false rule.
    n = 12345;
    test_check(!axl_json_get_string_len(&r, "num", &n),
        "jsonlen: a number is not a string");
    test_check(n == 12345, "jsonlen: out_len untouched when not a string");
    test_check(!axl_json_get_string_len(&r, "absent", &n),
        "jsonlen: a missing key returns false");
    test_check(!axl_json_get_string_len(NULL, "plain", &n),
        "jsonlen: NULL reader returns false");
    test_check(!axl_json_get_string_len(&r, "plain", NULL),
        "jsonlen: NULL out_len returns false");

    // The own-value form, which is what an ARRAY ELEMENT needs.
    AxlJsonArrayIter it;
    AxlJsonReader    elem;
    test_check(axl_json_array_begin(&r, "arr", &it), "jsonlen: array begins");
    bool elems_agree = true;
    const char *want[] = { "one", "aAb" };
    for (size_t i = 0; axl_json_array_next(&it, &elem); i++) {
        size_t elen = 0;
        if (!axl_json_value_string_len(&elem, &elen)) { elems_agree = false; break; }
        char got[32];
        axl_json_value_string(&elem, got, elen + 1);
        if (i >= 2 || axl_strlen(got) != elen || axl_strcmp(got, want[i]) != 0) {
            elems_agree = false;
        }
    }
    test_check(elems_agree,
        "jsonlen: value_string_len sizes array elements exactly");

    n = 777;
    AxlJsonReader numr;
    test_check(axl_json_get_value(&r, "num", &numr), "jsonlen: descend to num");
    test_check(!axl_json_value_string_len(&numr, &n),
        "jsonlen: value_string_len refuses a number");
    test_check(n == 777, "jsonlen: out_len untouched on refusal");

    axl_json_free(&r);

    // --- the object-key PEEK ------------------------------------------------
    // A key long enough that no sane fixed buffer would hold it, and one
    // carrying an escape, so a peek derived from the source span fails.
    const char *odoc =
        "{\"a-very-long-key-that-a-fixed-buffer-would-truncate\":1,"
        " \"esc\\u0041key\":2}";
    AxlJsonReader o;
    test_check(axl_json_parse(odoc, axl_strlen(odoc), AXL_JSON_STRICT, &o),
        "jsonlen: object fixture parses");

    AxlJsonObjectIter oit;
    test_check(axl_json_value_object_begin(&o, &oit), "jsonlen: object begins");

    const char *want_keys[] = {
        "a-very-long-key-that-a-fixed-buffer-would-truncate", "escAkey"
    };
    bool peek_ok = true;
    size_t pairs = 0;
    size_t klen = 0;
    while (axl_json_object_peek_key_len(&oit, &klen)) {
        char kbuf[128];
        AxlJsonReader v;
        if (klen + 1 > sizeof(kbuf)) { peek_ok = false; break; }
        if (!axl_json_object_next(&oit, kbuf, klen + 1, &v)) { peek_ok = false; break; }
        // The peek promised a size that cannot truncate, so the iterator's
        // per-pair error must be OK -- that is the whole contract.
        if (axl_json_object_iter_error(&oit)->code != AXL_JSON_OK) { peek_ok = false; }
        if (axl_strlen(kbuf) != klen) { peek_ok = false; }
        if (pairs < 2 && axl_strcmp(kbuf, want_keys[pairs]) != 0) { peek_ok = false; }
        pairs++;
    }
    test_check(peek_ok && pairs == 2,
        "jsonlen: peek sizes every key exactly, and none truncates");

    // The peek and the walk must agree about when the object is over: a peek
    // that stayed true past the last pair would spin forever in the loop
    // above, and one that went false early would drop a pair.
    test_check(!axl_json_object_peek_key_len(&oit, &klen),
        "jsonlen: peek is false once the walk is done");

    // It PEEKS -- calling it twice must not consume anything.
    AxlJsonObjectIter oit2;
    axl_json_value_object_begin(&o, &oit2);
    size_t k1 = 0, k2 = 0;
    axl_json_object_peek_key_len(&oit2, &k1);
    axl_json_object_peek_key_len(&oit2, &k2);
    test_check(k1 == k2 && k1 == axl_strlen(want_keys[0]),
        "jsonlen: peeking twice reports the same pair (it does not advance)");
    char after[128];
    AxlJsonReader av;
    axl_json_object_next(&oit2, after, sizeof(after), &av);
    test_check(axl_strcmp(after, want_keys[0]) == 0,
        "jsonlen: next() still yields the peeked pair");

    test_check(!axl_json_object_peek_key_len(NULL, &klen),
        "jsonlen: NULL iterator returns false");

    axl_json_free(&o);
}

// The double atom. The load-bearing assertion is the SPELLING: `%.17g` is
// claimed to be the shortest round-trippable form on this engine rather than
// literally 17 digits, and that claim rests on axl_dtoa producing at most 17
// shortest digits so the significant-digit rounding is a no-op. If that ever
// stops holding, 0.1 emits as 0.10000000000000001 and these fail.
static void
test_json_write_double(void)
{
    AXL_AUTOPTR(AxlString) out = axl_string_new("");
    AxlJsonWriter w;

    axl_json_writer_init(&w, out, AXL_JSON_STRICT);
    axl_json_arr_begin(&w);
    axl_json_double(&w, 0.1);
    axl_json_double(&w, 1.5);
    axl_json_double(&w, -2.25);
    axl_json_double(&w, 0.0);
    axl_json_double(&w, 1e300);
    axl_json_double(&w, 3.0);
    axl_json_arr_end(&w);
    axl_json_writer_finish(&w);

    test_check(axl_strcmp(axl_string_str(out),
                          "[0.1,1.5,-2.25,0,1e+300,3]") == 0,
        "jsondbl: shortest round-trip spelling, not 17 digits");
    test_check(!axl_json_writer_error(&w), "jsondbl: no writer error");

    // Round-trip through the READER: what was written must read back
    // BIT-IDENTICAL, which is the property "%.17g" is there to buy and which
    // an exact-string check alone does not prove.
    AxlJsonReader r;
    test_check(axl_json_parse(axl_string_str(out), axl_string_len(out),
                              AXL_JSON_STRICT, &r), "jsondbl: output reparses");
    const double want[] = { 0.1, 1.5, -2.25, 0.0, 1e300, 3.0 };
    AxlJsonArrayIter it;
    AxlJsonReader    elem;
    bool round_trips = true;
    size_t i = 0;
    if (axl_json_value_array_begin(&r, &it)) {
        while (axl_json_array_next(&it, &elem)) {
            double got = 0;
            if (i >= 6 || !axl_json_value_double(&elem, &got) || got != want[i]) {
                round_trips = false;
            }
            i++;
        }
    } else {
        round_trips = false;
    }
    test_check(round_trips && i == 6,
        "jsondbl: every value reads back BIT-IDENTICAL");
    axl_json_free(&r);

    // Non-finite is a DIALECT question. Strict refuses and latches; nothing is
    // emitted, because a `nan` token no reader accepts is worse than an error.
    AXL_AUTOPTR(AxlString) sout = axl_string_new("");
    AxlJsonWriter sw;
    axl_json_writer_init(&sw, sout, AXL_JSON_STRICT);
    axl_json_arr_begin(&sw);
    axl_json_double(&sw, 1.0 / 0.0);
    axl_json_arr_end(&sw);
    axl_json_writer_finish(&sw);
    test_check(axl_json_writer_error(&sw),
        "jsondbl: strict REFUSES infinity");

    // ...and the same bit that lets the READER accept them lets the writer
    // emit them. One flag, both directions.
    AXL_AUTOPTR(AxlString) nout = axl_string_new("");
    AxlJsonWriter nw;
    axl_json_writer_init(&nw, nout, AXL_JSON_ALLOW_NAN_INF);
    axl_json_arr_begin(&nw);
    axl_json_double(&nw, 1.0 / 0.0);
    axl_json_double(&nw, -1.0 / 0.0);
    axl_json_double(&nw, 0.0 / 0.0);
    axl_json_arr_end(&nw);
    axl_json_writer_finish(&nw);
    test_check(!axl_json_writer_error(&nw), "jsondbl: NAN_INF accepts them");
    test_check(axl_strcmp(axl_string_str(nout),
                          "[Infinity,-Infinity,NaN]") == 0,
        "jsondbl: JSON5 spelling, not the C library's nan/inf");

    AxlJsonReader nr;
    test_check(axl_json_parse(axl_string_str(nout), axl_string_len(nout),
                              AXL_JSON_ALLOW_NAN_INF, &nr),
        "jsondbl: AXL's own reader accepts what it wrote");
    axl_json_free(&nr);

    // kv form.
    AXL_AUTOPTR(AxlString) kout = axl_string_new("");
    AxlJsonWriter kw;
    axl_json_writer_init(&kw, kout, AXL_JSON_STRICT);
    axl_json_obj_begin(&kw);
    axl_json_kv_double(&kw, "temp", 36.6);
    axl_json_obj_end(&kw);
    axl_json_writer_finish(&kw);
    test_check(axl_strcmp(axl_string_str(kout), "{\"temp\":36.6}") == 0,
        "jsondbl: kv_double emits key and value");
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

    ok = axl_json_parse(json, axl_strlen(json), AXL_JSON_RELAXED, &r);
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
    ok = axl_json_parse(j5, axl_strlen(j5),
                              AXL_JSON_JSON5, &r);
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

    ok = axl_json_parse(j5, axl_strlen(j5),
                              AXL_JSON_JSON5, &r);
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

    // --- Integer overflow: reject, do not wrap --------------------------
    // Both accumulators ran unbounded into an int64_t: the hex branch as
    // `v = (v << 4) | n` and the decimal branch as `v = v * 10 + digit`.
    // Past the width that is silent wraparound to a WRONG value with no
    // diagnostic (and, being signed, undefined behaviour on the way there)
    // — the same shape as the signed-char bug 6ee757cd fixed. The sidecars
    // that ship in-tree hold 16-bit IDs, but --ids-file makes them
    // user-replaceable, so the input is not trusted.
    //
    // Boundary pairs: the largest value that must still parse, next to the
    // first that must not. A bound that is off by one fails exactly one of
    // each pair, which a single over-large case could not detect.
    struct { const char *json; bool want_ok; int64_t want; const char *what; } ovf[] = {
        { "{ v: 0x7FFFFFFFFFFFFFFF }",   true,  INT64_MAX, "hex INT64_MAX parses" },
        { "{ v: 0x8000000000000000 }",   false, 0, "hex INT64_MAX+1 rejected" },
        { "{ v: 0xFFFFFFFFFFFFFFFF }",   false, 0, "hex 2^64-1 rejected (wrapped to -1)" },
        { "{ v: 0x10000000000000000 }",  false, 0, "hex 2^64 rejected (wrapped to 0)" },
        { "{ v: -0x8000000000000000 }",  true,  INT64_MIN, "hex INT64_MIN parses" },
        { "{ v: -0x8000000000000001 }",  false, 0, "hex INT64_MIN-1 rejected" },
        { "{ v: 9223372036854775807 }",  true,  INT64_MAX, "decimal INT64_MAX parses" },
        { "{ v: 9223372036854775808 }",  false, 0, "decimal INT64_MAX+1 rejected" },
        { "{ v: -9223372036854775808 }", true,  INT64_MIN, "decimal INT64_MIN parses" },
        { "{ v: 99999999999999999999 }", false, 0, "decimal 20-digit rejected" },
    };
    for (size_t oi = 0; oi < sizeof(ovf) / sizeof(ovf[0]); oi++) {
        AxlJsonReader ovr;
        int64_t       got = 0x5A5A5A5A;   /* poison: a no-write must not pass */
        bool          parsed = axl_json_parse(ovf[oi].json,
                                                    axl_strlen(ovf[oi].json),
                                                    AXL_JSON_JSON5, &ovr);
        /* Assert the PARSE separately from the accessor: a rejection by the
         * JSON5 lexer would otherwise be indistinguishable from the bound
         * doing its job, and these cases would keep passing while testing
         * nothing if the lexer ever gained a digit-count cap. */
        test_check(parsed, ovf[oi].what);
        bool got_ok = parsed && axl_json_get_int(&ovr, "v", &got);
        if (parsed) axl_json_free(&ovr);
        /* On rejection *value must be untouched, so pin the poison too —
         * `want_ok ||` alone would leave the reject cases asserting nothing
         * about the out-param. */
        test_check(got_ok == ovf[oi].want_ok
                   && (ovf[oi].want_ok ? got == ovf[oi].want
                                       : got == 0x5A5A5A5A),
                   ovf[oi].what);
    }

    // Same bound through axl_json_parse — one parser now serves every
    // dialect, so this exercises the same decimal branch reached above and
    // the whole literal arrives as one primitive token either way. Kept
    // because the ENTRY POINT differs, and the bound is the accessor's.
    {
        const char *big = "{\"v\": 9223372036854775808}";
        AxlJsonReader sr;
        int64_t       sv = 0x5A5A5A5A;
        test_check(axl_json_parse(big, axl_strlen(big), AXL_JSON_RELAXED, &sr),
                   "json overflow: strict parser accepts the document");
        test_check(!axl_json_get_int(&sr, "v", &sv) && sv == 0x5A5A5A5A,
                   "json overflow: strict path rejects INT64_MAX+1 too");
        axl_json_free(&sr);
    }

    // axl_json_get_uint takes a uint64_t*, so the whole unsigned range is
    // representable in its out-param — it used to route through int64_t and
    // silently cap at INT64_MAX, which is exactly the half a firmware sidecar
    // needs for a 64-bit mask or physical address.
    {
        struct { const char *json; bool want_ok; uint64_t want; const char *what; } u[] = {
            { "{ v: 0xFFFFFFFFFFFFFFFF }",   true,  UINT64_MAX, "uint: hex UINT64_MAX parses" },
            { "{ v: 18446744073709551615 }", true,  UINT64_MAX, "uint: decimal UINT64_MAX parses" },
            { "{ v: 0x10000000000000000 }",  false, 0, "uint: 2^64 rejected" },
            { "{ v: 18446744073709551616 }", false, 0, "uint: decimal 2^64 rejected" },
            { "{ v: 0x8000000000000000 }",   true,  (uint64_t)INT64_MAX + 1u,
              "uint: reaches past INT64_MAX" },
            { "{ v: -1 }",                   false, 0, "uint: negative rejected" },
        };
        for (size_t ui = 0; ui < sizeof(u) / sizeof(u[0]); ui++) {
            AxlJsonReader ur;
            uint64_t      got = 0x5A5A5A5A5A5A5A5AULL;
            bool parsed = axl_json_parse(u[ui].json, axl_strlen(u[ui].json),
                                               AXL_JSON_JSON5, &ur);
            test_check(parsed, u[ui].what);
            bool got_ok = parsed && axl_json_get_uint(&ur, "v", &got);
            if (parsed) axl_json_free(&ur);
            test_check(got_ok == u[ui].want_ok
                       && (u[ui].want_ok ? got == u[ui].want
                                         : got == 0x5A5A5A5A5A5A5A5AULL),
                       u[ui].what);
        }
    }

    // Trailing comma in array, hex with negative, x-escape in string
    const char *j5_arr =
        "{ items: [1, 2, 0x10, -0xFF,], greeting: \"hi\\x21\", }";
    ok = axl_json_parse(j5_arr, axl_strlen(j5_arr),
                              AXL_JSON_JSON5, &r);
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

    /* STRICT must still reject JSON5 input -- but it has to be ASKED for now.
       This used to call axl_json_parse(), which is no longer the strict entry
       point: it means AXL_JSON_RELAXED, so it accepts this document. Left
       pointing at the same document via the flags form, because "strict
       rejects JSON5" is exactly the property worth keeping an assertion on;
       only the way to request strictness changed. */
    ok = axl_json_parse(j5, axl_strlen(j5), AXL_JSON_STRICT, &r);
    test_check(!ok, "json5 parse: STRICT still rejects JSON5");

    /* And the other half of that change, or "strict rejects it" would pass
       against a parser that rejected the document for everyone. */
    ok = axl_json_parse(j5, axl_strlen(j5), AXL_JSON_RELAXED, &r);
    test_check(ok, "json5 parse: AXL_JSON_RELAXED accepts it");
    if (ok) { axl_json_free(&r); }

    // Default flags == strict
    ok = axl_json_parse(j5, axl_strlen(j5),
                              AXL_JSON_STRICT, &r);
    test_check(!ok, "json5 parse: AXL_JSON_STRICT == strict");

    // Strict JSON parses correctly via the JSON5 path too (superset)
    const char *strict = "{\"a\":1,\"b\":[true,null]}";
    ok = axl_json_parse(strict, axl_strlen(strict),
                              AXL_JSON_JSON5, &r);
    test_check(ok, "json5 parse: accepts strict JSON unchanged");
    ok = axl_json_get_int(&r, "a", &int_val);
    test_check(ok && int_val == 1, "json5 parse: strict accessors work");
    axl_json_free(&r);

    // Malformed JSON5 fails (unterminated block comment)
    const char *bad = "{ /* never closed \n a: 1 }";
    ok = axl_json_parse(bad, axl_strlen(bad),
                              AXL_JSON_JSON5, &r);
    test_check(!ok, "json5 parse: unterminated block comment rejected");
}

// ---------------------------------------------------------------------------
// \uXXXX decoding in the string ACCESSORS (regression, shipped in v3.1.0)
//
// decode_json_string had no `case 'u':`, so every \uXXXX fell through to the
// default "copy the character after the backslash" arm: "\u0041" came back as
// "u0041". The lexer validates the escape, so the document parsed, no error
// was raised, and the caller got wrong bytes — accept-and-corrupt, the worst
// shape a parser bug takes.
//
// It survived because both existing \u tests exercise the WRITER (which
// splices escape bytes verbatim, correctly), never the decoder. Every
// get_string test used escape-free ASCII.
//
// Exact bytes throughout — a substring check would accept "u0041" inside a
// longer string, which is the very failure being pinned.
// ---------------------------------------------------------------------------

static void
test_json_unicode_escapes(void)
{
    AxlJsonReader r;
    char          buf[64];

    // One case, one document. Parse strictly unless noted; \uXXXX is valid in
    // both dialects and both route through the same decoder.
    #define U_CASE(doc, want, msg)                                            \
        do {                                                                  \
            const char *_d = (doc);                                           \
            axl_memset(buf, 0, sizeof(buf));                                  \
            test_check(axl_json_parse(_d, axl_strlen(_d), AXL_JSON_RELAXED, &r)                 \
                       && axl_json_get_string(&r, "a", buf, sizeof(buf))      \
                       && axl_strcmp(buf, (want)) == 0, (msg));               \
            axl_json_free(&r);                                                \
        } while (0)

    U_CASE("{\"a\":\"\\u0041\"}",  "A",            "json \\u: basic BMP decodes to 1-byte UTF-8");
    U_CASE("{\"a\":\"x\\u0041y\"}", "xAy",         "json \\u: mid-string, neighbours intact");
    U_CASE("{\"a\":\"\\u00e9\"}",  "\xc3\xa9",     "json \\u: U+00E9 is 2-byte UTF-8");
    U_CASE("{\"a\":\"\\u20ac\"}",  "\xe2\x82\xac", "json \\u: U+20AC is 3-byte UTF-8");
    U_CASE("{\"a\":\"\\uFFFF\"}",  "\xef\xbf\xbf", "json \\u: max BMP is still 3 bytes");

    // Surrogate PAIR combines into one code point above the BMP. Decoding the
    // halves separately would give two 3-byte sequences, not this.
    U_CASE("{\"a\":\"\\ud83d\\ude00\"}", "\xf0\x9f\x98\x80",
           "json \\u: surrogate pair combines to U+1F600 (4-byte UTF-8)");
    U_CASE("{\"a\":\"A\\ud83d\\ude00B\"}", "A\xf0\x9f\x98\x80" "B",
           "json \\u: surrogate pair mid-string keeps its neighbours");

    // Lone surrogates are ill-formed UTF-16. The parse side accepts them
    // (JSONTestSuite classifies these i_), so the accessor is what must never
    // hand back ill-formed UTF-8: substitute U+FFFD.
    U_CASE("{\"a\":\"\\ud83d\"}",  "\xef\xbf\xbd", "json \\u: lone HIGH surrogate becomes U+FFFD");
    U_CASE("{\"a\":\"\\udc00\"}",  "\xef\xbf\xbd", "json \\u: bare LOW surrogate becomes U+FFFD");
    U_CASE("{\"a\":\"\\ud83dZ\"}", "\xef\xbf\xbd" "Z",
           "json \\u: high surrogate not followed by \\u becomes U+FFFD, Z kept");
    U_CASE("{\"a\":\"\\ud83d\\u0041\"}", "\xef\xbf\xbd" "A",
           "json \\u: high surrogate followed by a NON-surrogate \\u keeps both");

    // \u0000 must never become an interior NUL: the buffer is NUL-terminated,
    // so one would truncate the string for every axl_strcmp caller and make
    // "admin\u0000extra" compare equal to "admin" — a string-smuggling
    // primitive reachable from JWT/JWK/HTTP input.
    U_CASE("{\"a\":\"\\u0000\"}",   "\xef\xbf\xbd", "json \\u: \\u0000 becomes U+FFFD, not a NUL");
    U_CASE("{\"a\":\"a\\u0000b\"}", "a\xef\xbf\xbd" "b",
           "json \\u: \\u0000 mid-string keeps both neighbours");

    // The smuggling assertion stated directly: length must be 5, not 1.
    axl_memset(buf, 0, sizeof(buf));
    const char *nul_doc = "{\"a\":\"a\\u0000b\"}";
    test_check(axl_json_parse(nul_doc, axl_strlen(nul_doc), AXL_JSON_RELAXED, &r)
               && axl_json_get_string(&r, "a", buf, sizeof(buf))
               && axl_strlen(buf) == 5,
               "json \\u: \\u0000 does not truncate the decoded string");
    axl_json_free(&r);

    // JSON5 routes through the same decoder — pin it so a dialect split can't
    // silently regress one side.
    axl_memset(buf, 0, sizeof(buf));
    const char *j5 = "{a:'\\u20ac'}";
    test_check(axl_json_parse(j5, axl_strlen(j5), AXL_JSON_JSON5, &r)
               && axl_json_get_string(&r, "a", buf, sizeof(buf))
               && axl_strcmp(buf, "\xe2\x82\xac") == 0,
               "json \\u: JSON5 mode decodes \\uXXXX identically");
    axl_json_free(&r);

    // The OTHER affected accessor. axl_json_value_string shares the decoder,
    // so it must agree — including on the pair case.
    AxlJsonArrayIter it;
    AxlJsonReader    elem;
    const char      *arr = "{\"a\":[\"\\u00e9\",\"\\ud83d\\ude00\"]}";
    char             e0[32] = {0}, e1[32] = {0};
    bool             got0 = false, got1 = false;
    if (axl_json_parse(arr, axl_strlen(arr), AXL_JSON_RELAXED, &r)
        && axl_json_array_begin(&r, "a", &it)) {
        if (axl_json_array_next(&it, &elem)) got0 = axl_json_value_string(&elem, e0, sizeof(e0));
        if (axl_json_array_next(&it, &elem)) got1 = axl_json_value_string(&elem, e1, sizeof(e1));
    }
    test_check(got0 && axl_strcmp(e0, "\xc3\xa9") == 0,
               "json \\u: value_string decodes a BMP escape");
    test_check(got1 && axl_strcmp(e1, "\xf0\x9f\x98\x80") == 0,
               "json \\u: value_string combines a surrogate pair");
    axl_json_free(&r);

    // --- buffer bound -------------------------------------------------------
    // A decoded code point is 1-4 bytes but the loop guard only reserves ONE,
    // so the new branch must check before writing. Truncation is the existing
    // convention for an over-long value (the plain-byte path has always
    // truncated silently and returned true); what must NEVER happen is a
    // PARTIAL UTF-8 sequence, which is the same ill-formed output the
    // lone-surrogate rule exists to prevent.
    const char *emoji = "{\"a\":\"\\ud83d\\ude00\"}";   // 4 bytes + NUL = 5

    char exact[5];
    axl_memset(exact, 0, sizeof(exact));
    test_check(axl_json_parse(emoji, axl_strlen(emoji), AXL_JSON_RELAXED, &r)
               && axl_json_get_string(&r, "a", exact, sizeof(exact))
               && axl_strcmp(exact, "\xf0\x9f\x98\x80") == 0,
               "json \\u: a buffer of exactly 5 holds the 4-byte code point");
    axl_json_free(&r);

    char tight[4];   // one byte too small for the code point + NUL
    axl_memset(tight, 0xAA, sizeof(tight));
    test_check(axl_json_parse(emoji, axl_strlen(emoji), AXL_JSON_RELAXED, &r)
               && axl_json_get_string(&r, "a", tight, sizeof(tight))
               && axl_strcmp(tight, "") == 0,
               "json \\u: a too-small buffer emits NO partial sequence");
    axl_json_free(&r);

    // A zero-size buffer. Before the fix the guard `out < dst_size - 1`
    // underflowed to SIZE_MAX here and the terminator was written out of
    // bounds; there is no in-tree caller that passes 0, but the accessor is
    // public. Canary the byte after so a stray write is visible.
    char zero[2];
    zero[0] = (char)0x5A;
    zero[1] = (char)0x5A;
    test_check(axl_json_parse(emoji, axl_strlen(emoji), AXL_JSON_RELAXED, &r)
               && !axl_json_get_string(&r, "a", zero, 0)
               && zero[0] == (char)0x5A,
               "json \\u: a zero-size buffer fails and writes nothing");
    axl_json_free(&r);

    // Same bound, but with content before it — the prefix survives and the
    // code point that would overflow is dropped whole, not split.
    char part[6];
    axl_memset(part, 0xAA, sizeof(part));
    const char *pre = "{\"a\":\"ab\\ud83d\\ude00\"}";
    test_check(axl_json_parse(pre, axl_strlen(pre), AXL_JSON_RELAXED, &r)
               && axl_json_get_string(&r, "a", part, sizeof(part))
               && axl_strcmp(part, "ab") == 0,
               "json \\u: an overflowing code point is dropped whole, prefix kept");
    axl_json_free(&r);

    #undef U_CASE
}

// ---------------------------------------------------------------------------
// Three spellings of an unrepresentable NUL, ONE overflow policy
//
// decode_json_string is where two independent fixes met: main's \uXXXX decode
// (0352d185) and this branch's JSON5 \0 / \x00 hardening. Both substitute
// U+FFFD, and both were right about that — but they disagreed on what happens
// when the substitution does not FIT.
//
// The \u arm drops the code point whole and stops the loop. The \0 and \x00
// arms emitted nothing and KEPT GOING, so a tight buffer made the replacement
// vanish from the MIDDLE of a string while its successors survived:
// "a\0b" came back as "ab". That reads \0 as "nothing" rather than
// "unrepresentable" — a quieter version of the same smuggling primitive the
// substitution exists to block, and it differs from plain truncation, which is
// this accessor's documented convention for a value that will not fit.
//
// So the three spellings are asserted SIDE BY SIDE rather than each in its own
// section: the claim being pinned is that they agree, and a per-spelling test
// cannot express that. \x00 had no test at all on either branch.
// ---------------------------------------------------------------------------

static void
test_json_nul_escape_union(void)
{
    // Three documents differing only in HOW the NUL is spelled. The \u form is
    // legal in every dialect; \0 and \x00 are JSON5 and need
    // ALLOW_EXTRA_ESCAPES, which AXL_JSON_RELAXED carries. All three must
    // reach the same answer through the same decoder.
    //
    // Three buffer sizes per spelling, because the disagreement was entirely
    // about the bound:
    //   32 - everything fits
    //    5 - 'a' + the 3-byte U+FFFD + NUL, so the trailing 'b' does not
    //    4 - one byte short of the substitution itself
    //
    // Split literals throughout: "\xBDb" would lex as ONE out-of-range hex
    // escape and swallow the 'b', since 'b' is itself a hex digit.
    struct { const char *doc; size_t size; const char *want; const char *msg; }
    row[] = {
        { "{\"a\":\"a\\u0000b\"}", 32, "a\xEF\xBF\xBD" "b",
          "nul union: \\u0000 decodes to U+FFFD, exact, no truncation" },
        { "{\"a\":\"a\\0b\"}",     32, "a\xEF\xBF\xBD" "b",
          "nul union: \\0 decodes to U+FFFD, exact, no truncation" },
        { "{\"a\":\"a\\x00b\"}",   32, "a\xEF\xBF\xBD" "b",
          "nul union: \\x00 decodes to U+FFFD, exact, no truncation" },

        { "{\"a\":\"a\\u0000b\"}",  5, "a\xEF\xBF\xBD",
          "nul union: \\u0000 fills a 5-byte buffer, 'b' is cut" },
        { "{\"a\":\"a\\0b\"}",      5, "a\xEF\xBF\xBD",
          "nul union: \\0 fills a 5-byte buffer, 'b' is cut" },
        { "{\"a\":\"a\\x00b\"}",    5, "a\xEF\xBF\xBD",
          "nul union: \\x00 fills a 5-byte buffer, 'b' is cut" },

        { "{\"a\":\"a\\u0000b\"}",  4, "a",
          "nul union: \\u0000 too big for the buffer truncates THERE" },
        { "{\"a\":\"a\\0b\"}",      4, "a",
          "nul union: \\0 too big for the buffer truncates THERE "
          "(it must not vanish and let 'b' through)" },
        { "{\"a\":\"a\\x00b\"}",    4, "a",
          "nul union: \\x00 too big for the buffer truncates THERE "
          "(it must not vanish and let 'b' through)" },

        /* The SINGLE-QUOTED, unquoted-key spelling, carried over from the
           equivalent test `main` grew independently (aba93d54) while this
           branch was doing the same work. That test was dropped in the merge
           -- it is written against AXL_JSON_PARSER_JSON5, a constant P1
           removed, and its claims are the rows above -- but it reached the
           decoder through a different lexer path, and losing an input SHAPE is
           a real coverage loss even when the assertion is a duplicate. Its
           "admin"/"extra" payload is kept verbatim: the point of that wording
           is that a truncated read compares equal to "admin". */
        { "{a:'admin\\0extra'}",   32, "admin\xEF\xBF\xBD" "extra",
          "nul union: single-quoted \\0 keeps both neighbours, so a "
          "truncated read cannot pass for \"admin\"" },
        { "{a:'admin\\x00extra'}", 32, "admin\xEF\xBF\xBD" "extra",
          "nul union: single-quoted \\x00 keeps both neighbours too" },
    };
    size_t n = sizeof(row) / sizeof(row[0]);
    size_t i;

    for (i = 0; i < n; i++) {
        AxlJsonReader r;
        char          buf[32];

        // The 0xAA prefill is load-bearing twice: a missing terminator makes
        // axl_strcmp run into it and mismatch, and the byte AT the declared
        // size catches a one-past-the-end terminator, which is the exact bug
        // the removed `dst_size - 1` underflow used to cause.
        axl_memset(buf, (char)0xAA, sizeof(buf));
        test_check(axl_json_parse(row[i].doc, axl_strlen(row[i].doc), AXL_JSON_RELAXED, &r)
                   && axl_json_get_string(&r, "a", buf, row[i].size)
                   && axl_strcmp(buf, row[i].want) == 0
                   && (row[i].size == sizeof(buf)
                       || buf[row[i].size] == (char)0xAA),
                   row[i].msg);
        axl_json_free(&r);
    }
}

// ---------------------------------------------------------------------------
// JSON5 line continuations, including the CRLF PAIR
//
// ES5 LineContinuation is `\` followed by a LineTerminatorSequence, and that
// sequence is <LF>, <CR>, <LS>, <PS>, or the PAIR <CR><LF>. The lexer consumed
// exactly TWO bytes for any `\<anychar>`, so `\<CR><LF>` left the LF behind as
// a raw character -- which then tripped the "no raw control character in a
// string" rule, and the whole document was rejected.
//
// decode_json_string already handled the pair correctly (`case '\r'` consumes
// a following LF), so the two halves disagreed: the decoder knew CRLF was one
// terminator and the lexer did not. Nothing caught it because a CRLF document
// is awkward to write as a C literal and no test had one.
//
// Found by json5/json5-tests on that corpus's FIRST run through
// test-json-corpus-qemu.sh -- exactly the class of gap an external suite
// exists to close, and one no amount of reading our own code had surfaced.
// ---------------------------------------------------------------------------

static void
test_json5_line_continuations(void)
{
    struct { const char *doc; const char *want; const char *msg; } row[] = {
        { "{a:'line 1 \\\nline 2'}", "line 1 line 2",
          "continuation: \\<LF> joins the lines" },
        { "{a:'line 1 \\\rline 2'}", "line 1 line 2",
          "continuation: \\<CR> joins the lines" },
        /* The regression. <CR><LF> is ONE line terminator, so the escape must
           consume all three bytes; consuming two leaves a raw LF behind. */
        { "{a:'line 1 \\\r\nline 2'}", "line 1 line 2",
          "continuation: \\<CR><LF> is ONE terminator, not CR plus a raw LF" },
    };
    size_t n = sizeof(row) / sizeof(row[0]);
    size_t i;

    for (i = 0; i < n; i++) {
        AxlJsonReader r;
        char          buf[64];

        axl_memset(buf, 0, sizeof(buf));
        test_check(axl_json_parse(row[i].doc, axl_strlen(row[i].doc),
                                        AXL_JSON_JSON5, &r)
                   && axl_json_get_string(&r, "a", buf, sizeof(buf))
                   && axl_strcmp(buf, row[i].want) == 0,
                   row[i].msg);
        axl_json_free(&r);
    }

    /* The other half, and the reason this is a pair of assertions rather than
       one: widening the continuation rule must NOT make a raw newline legal
       inside a string. Both RFC 8259 and ES5 forbid that, and a fix that
       simply stopped checking control characters would pass every row above. */
    {
        const char   *raw = "{a:'line 1 \nline 2'}";
        AxlJsonReader r;
        bool          got = axl_json_parse(raw, axl_strlen(raw),
                                                 AXL_JSON_JSON5, &r);
        if (got) { axl_json_free(&r); }
        test_check(!got,
                   "continuation: a RAW newline in a string is still rejected");
    }
}

// ---------------------------------------------------------------------------
// An accessor never hands back a BROKEN code point
//
// Two holes the \0/\uXXXX reconciliation left behind, both found by the review
// pass, and both about input the decoder fully understands -- so neither is
// P7's read-side UTF-8 validation work:
//
//  1. `\xNN` emitted a RAW byte. JSON5 inherits ES5's HexEscapeSequence, where
//     `\xE9` is the code UNIT U+00E9 and encodes as the two bytes C3 A9. AXL
//     emitted a lone 0xE9, which is not valid UTF-8 in any encoding -- an
//     ESCAPE producing ill-formed output, from a decoder that had just grown a
//     helper whose whole purpose is to prevent that. The two existing \x
//     assertions pin \x21 and \x41, both ASCII, so nothing noticed.
//
//  2. Truncation split a multi-byte sequence. `{"a":"<U+20AC>"}` into three
//     bytes gave E2 82 + NUL -- the same ill-formed output append_bytes
//     refuses for a decoded escape, on the path that carries essentially all
//     real text.
//
// Hole 2 took two attempts, and the difference is what the rows below are
// organized around. The first attempt made each raw RUN atomic on the way in
// (measure a lead byte plus its continuation bytes, refuse the run whole). That
// fixed the common case and missed the general one: two ADJACENT one-byte units
// can concatenate into a sequence no arm ever saw as a unit -- `\<C3>\<A9>`
// (each byte escaped separately, what a naive byte-oriented escaper emits for
// U+00E9) or `<C3>\<80>` (raw lead, escaped continuation). A cut between those
// two units still split a sequence the untruncated decode had whole.
//
// So the mechanism is now ONE post-hoc trim over the bytes actually written,
// and the atomic-run measurement is gone as redundant -- every truncation row
// below fails when the trim is removed, which is what proved it. Consequence
// worth knowing: no row here discriminates "grouping", because there is no
// grouping and no lookahead at all.
//
// What is NOT changed, and is asserted so the fix cannot creep into it:
// ill-formed RAW bytes that FIT still pass through untouched. The reader
// validates no encoding (see AXL_JSON_UTF8_* on read, P7). The trim only ever
// removes a sequence AXL's own bound cut in half.
// ---------------------------------------------------------------------------

static void
test_json_accessor_utf8_integrity(void)
{
    struct { const char *doc; size_t size; const char *want; const char *msg; }
    row[] = {
        // --- 1. \xNN is a code unit, not a byte -----------------------------
        { "{\"a\":\"\\x21\"}", 32, "!",
          "utf8 integrity: \\x21 is ASCII, one byte, unchanged" },
        { "{\"a\":\"\\xe9\"}", 32, "\xC3\xA9",
          "utf8 integrity: \\xe9 is code unit U+00E9, encoded as 2 bytes" },
        { "{\"a\":\"\\xff\"}", 32, "\xC3\xBF",
          "utf8 integrity: \\xff is code unit U+00FF, not a lone 0xFF" },
        { "{\"a\":\"\\xe9\"}",  2, "",
          "utf8 integrity: \\xe9 that will not fit is refused WHOLE" },
        { "{\"a\":\"\\xe9\"}",  3, "\xC3\xA9",
          "utf8 integrity: \\xe9 fits in exactly 3 bytes" },

        // --- 2. a raw sequence is never split by OUR bound -----------------
        // U+20AC EURO SIGN, 3 bytes. Hex escapes, never literal UTF-8 in a C
        // literal -- check-ascii forbids that.
        { "{\"a\":\"\xE2\x82\xAC\"}", 32, "\xE2\x82\xAC",
          "utf8 integrity: a raw 3-byte sequence survives a big buffer" },
        { "{\"a\":\"\xE2\x82\xAC\"}",  4, "\xE2\x82\xAC",
          "utf8 integrity: a raw 3-byte sequence fits in exactly 4 bytes" },
        { "{\"a\":\"\xE2\x82\xAC\"}",  3, "",
          "utf8 integrity: a raw 3-byte sequence is refused WHOLE, not split" },
        { "{\"a\":\"a\xE2\x82\xAC\"}", 4, "a",
          "utf8 integrity: an overflowing raw sequence truncates after 'a'" },
        { "{\"a\":\"\xC3\xA9\"}",       2, "",
          "utf8 integrity: a raw 2-byte sequence is refused WHOLE" },
        { "{\"a\":\"\xC3\xA9\"}",       3, "\xC3\xA9",
          "utf8 integrity: a raw 2-byte sequence fits in exactly 3 bytes" },

        // --- 3. ill-formed RAW bytes are still passed through --------------
        // The reader validates no encoding, so none of these changes shape.
        { "{\"a\":\"\x80\"}",     32, "\x80",
          "utf8 integrity: a raw orphan continuation byte passes through" },
        { "{\"a\":\"\xC3\"}",     32, "\xC3",
          "utf8 integrity: a raw truncated lead byte passes through" },
        { "{\"a\":\"\xC3z\"}",    32, "\xC3" "z",
          "utf8 integrity: a lead byte with no continuation keeps the next "
          "character" },

        // --- 4. an ESCAPED lead byte, continuations unescaped ---------------
        // JSON5's `\<anychar>` rule (ES5 NonEscapeCharacter) lets any byte be
        // escaped, and an ES5 escape covers a CHARACTER, not a byte -- so
        // `\<C3><A9>` is one escaped U+00E9 written across two source bytes.
        // Found by a host-side property test over 1.15M (input, buffer-size)
        // pairs; no hand-written case in this file reached it.
        //
        // Only the size-2 row discriminates. The untruncated output is the same
        // two bytes in the same order however the decoder gets there, so the 32-
        // and 3-byte rows are regression cover. Verified by sabotage.
        { "{\"a\":\"\\\xC3\xA9\"}", 32, "\xC3\xA9",
          "utf8 integrity: an escaped lead byte + its continuation decode as "
          "one character" },
        { "{\"a\":\"\\\xC3\xA9\"}",  3, "\xC3\xA9",
          "utf8 integrity: an escaped 2-byte character fits in exactly 3 bytes" },
        { "{\"a\":\"\\\xC3\xA9\"}",  2, "",
          "utf8 integrity: an escaped 2-byte character is refused WHOLE, not "
          "split after the lead" },

        // --- 5. two ADJACENT one-byte units can form a sequence -------------
        // The case that killed the atomic-run approach, because no single arm
        // ever sees these as one unit:
        //
        //   \<C3>\<A9>   both bytes escaped separately -- what a naive
        //                byte-oriented escaper emits for U+00E9
        //   <C3>\<80>    a raw lead byte, then an escaped continuation
        //
        // Truncation BETWEEN the two units split a sequence the untruncated
        // output had whole. It does NOT matter that these sources are themselves
        // ill-formed UTF-8: the test is whether truncation introduced
        // ill-formedness the full decode did not have. An earlier version of
        // this reasoning waved exactly these away as "ill-formed source, so
        // pass-through applies" -- wrong, because pass-through is about bytes
        // AXL was HANDED, not about a sequence AXL assembled and then broke.
        { "{\"a\":\"\\\xC3\\\xA9\"}", 32, "\xC3\xA9",
          "utf8 integrity: two separately-escaped bytes decode to one "
          "character" },
        { "{\"a\":\"\\\xC3\\\xA9\"}",  3, "\xC3\xA9",
          "utf8 integrity: two separately-escaped bytes fit in exactly 3" },
        { "{\"a\":\"\\\xC3\\\xA9\"}",  2, "",
          "utf8 integrity: a cut BETWEEN two separately-escaped bytes trims "
          "the half-sequence" },
        { "{\"a\":\"\xC3\\\x80\"}",   32, "\xC3\x80",
          "utf8 integrity: a raw lead plus an escaped continuation decodes "
          "whole" },
        { "{\"a\":\"\xC3\\\x80\"}",    2, "",
          "utf8 integrity: a cut between a raw lead and an escaped "
          "continuation trims the half-sequence" },
        // Four bytes, each escaped on its own: U+1F600 needs 5 with the NUL.
        { "{\"a\":\"\\\xF0\\\x9F\\\x98\\\x80\"}", 32, "\xF0\x9F\x98\x80",
          "utf8 integrity: four separately-escaped bytes decode to one "
          "4-byte character" },
        { "{\"a\":\"\\\xF0\\\x9F\\\x98\\\x80\"}",  4, "",
          "utf8 integrity: a 4-byte character escaped byte-by-byte is trimmed "
          "whole, not left as 3 bytes" },

        // --- 6. trim only what WE cut, never a byte that simply fit ---------
        // The trim looks at bytes already written, so on its own it cannot tell
        // "this lead byte was cut off from its continuations" from "this lead
        // byte never had any". It guessed the former and threw away a byte that
        // legitimately fit -- inconsistent with this file's own 32-byte row
        // asserting that `<C3>z` keeps BOTH bytes: at size 32 the 0xC3 is
        // legitimate output, so at size 2 it must still be.
        //
        // Fixed by asking the SOURCE what the next decoded byte would be, and
        // trimming only when it is a continuation byte. Exhaustively measured
        // at ~17% of truncation boundaries over an adversarial byte alphabet
        // before the fix -- data loss, though never an ill-formed emission.
        { "{\"a\":\"\xC3z\"}",        2, "\xC3",
          "utf8 integrity: a lead byte whose successor CANNOT complete it "
          "survives truncation" },
        { "{\"a\":\"ab\xE0XY\"}",     4, "ab\xE0",
          "utf8 integrity: a 3-byte lead followed by non-continuations is kept "
          "at the bound" },
        // The other side of the same rule: a COMPLETE sequence must not be
        // trimmed just because a continuation byte happens to follow it.
        { "{\"a\":\"\xC3\xA9\x80\"}", 3, "\xC3\xA9",
          "utf8 integrity: a complete sequence is kept when an orphan "
          "continuation follows" },

        // A raw high byte immediately before an ESCAPE. This row was written to
        // discriminate the atomic-run version's one subtle rule -- count the
        // continuation bytes that are THERE, never the count the lead byte
        // declares, or the following `\` gets swallowed and stops being an
        // escape. That mechanism is gone, and with no lookahead the hazard
        // cannot recur, so the row is now regression cover rather than a
        // discriminator. Kept deliberately: it is the shape that would break
        // first if lookahead is ever reintroduced.
        { "{\"a\":\"\xC3\\n\"}",  32, "\xC3\n",
          "utf8 integrity: a raw high byte before an escape leaves the escape "
          "intact" },
    };
    size_t n = sizeof(row) / sizeof(row[0]);
    size_t i;

    for (i = 0; i < n; i++) {
        AxlJsonReader r;
        char          buf[32];

        axl_memset(buf, (char)0xAA, sizeof(buf));
        test_check(axl_json_parse(row[i].doc, axl_strlen(row[i].doc), AXL_JSON_RELAXED, &r)
                   && axl_json_get_string(&r, "a", buf, row[i].size)
                   && axl_strcmp(buf, row[i].want) == 0
                   && (row[i].size == sizeof(buf)
                       || buf[row[i].size] == (char)0xAA),
                   row[i].msg);
        axl_json_free(&r);
    }
}

// ---------------------------------------------------------------------------
// P9 — a failure says WHAT went wrong, WHERE, and how to fix it
//
// Three claims, each with its own rows below.
//
//  1. The CODE distinguishes classes a caller acts on differently. The
//     load-bearing split is INCOMPLETE vs UNEXPECTED_BYTE: "send more bytes"
//     and "this will never parse" are opposite instructions.
//
//  2. The POSITION is usable. offset is a byte index; column counts
//     CHARACTERS, so a caret lines up on a line with non-ASCII before the
//     error. Byte and character columns are equal on ASCII, so a test that
//     only uses ASCII cannot tell them apart -- hence the multibyte row.
//
//  3. A DIALECT miss names the flag that would have accepted it. This is the
//     only recoverable class, and naming the flag is what makes it so.
//
// Three sites had to be restructured to make claim 3 true rather than
// mostly-true; they tested the flag BEFORE recognising the feature, so by the
// time they failed they no longer knew what had been attempted. The tell was
// an asymmetry -- `-NaN` reported a dialect miss and `NaN` reported "unknown
// literal", for one flag. Those rows are marked.
// ---------------------------------------------------------------------------

static void
test_json_error_reporting(void)
{
    struct {
        const char      *doc;
        AxlJsonFlags     flags;
        AxlJsonErrorCode code;
        AxlJsonFlags     flag;   /* expected missing_flag, 0 if not DIALECT */
        const char      *msg;
    } row[] = {
        // --- INCOMPLETE: ran out of input --------------------------------
        { "{\"a\":1",        AXL_JSON_STRICT, AXL_JSON_ERR_INCOMPLETE, 0,
          "err: unterminated object is INCOMPLETE" },
        { "[1,2",            AXL_JSON_STRICT, AXL_JSON_ERR_INCOMPLETE, 0,
          "err: unterminated array is INCOMPLETE" },
        { "\"abc",           AXL_JSON_STRICT, AXL_JSON_ERR_INCOMPLETE, 0,
          "err: unterminated string is INCOMPLETE" },
        { "{\"a\":\"\\u00",  AXL_JSON_STRICT, AXL_JSON_ERR_INCOMPLETE, 0,
          "err: \\u escape running off the end is INCOMPLETE, not BAD_ESCAPE" },
        { "{\"a\":1}/*",     AXL_JSON_JSON5,  AXL_JSON_ERR_INCOMPLETE, 0,
          "err: unterminated block comment is INCOMPLETE" },

        // --- BAD_ESCAPE: the other half of the same two sites -------------
        { "{\"a\":\"\\uZZZZ\"}", AXL_JSON_STRICT, AXL_JSON_ERR_BAD_ESCAPE, 0,
          "err: \\u with non-hex digits is BAD_ESCAPE, not INCOMPLETE" },
        { "{a:'\\xZZ'}",     AXL_JSON_JSON5,  AXL_JSON_ERR_BAD_ESCAPE, 0,
          "err: \\x with non-hex digits is BAD_ESCAPE" },

        // --- BAD_NUMBER ---------------------------------------------------
        { "{\"a\":01}",      AXL_JSON_STRICT, AXL_JSON_ERR_BAD_NUMBER, 0,
          "err: leading zero is BAD_NUMBER" },
        { "{\"a\":1e}",      AXL_JSON_STRICT, AXL_JSON_ERR_BAD_NUMBER, 0,
          "err: empty exponent is BAD_NUMBER" },

        // --- UNEXPECTED_BYTE: no flag can rescue these --------------------
        { "{\"a\":tru}",     AXL_JSON_STRICT, AXL_JSON_ERR_UNEXPECTED_BYTE, 0,
          "err: a broken literal is UNEXPECTED_BYTE" },
        { "{\"a\":\"x\ny\"}", AXL_JSON_JSON5, AXL_JSON_ERR_UNEXPECTED_BYTE, 0,
          "err: a raw LF in a string is UNEXPECTED_BYTE (no flag allows it)" },

        // --- TRAILING -----------------------------------------------------
        { "{} junk",         AXL_JSON_STRICT, AXL_JSON_ERR_TRAILING, 0,
          "err: content after a complete value is TRAILING" },

        // --- DEPTH --------------------------------------------------------
        { "[[[[[1]]]]]",     AXL_JSON_STRICT | AXL_JSON_DEPTH(2),
          AXL_JSON_ERR_DEPTH, 0, "err: past the depth bound is DEPTH" },

        // --- DIALECT: every one names its flag ----------------------------
        { "{\"a\":1}//c",    AXL_JSON_STRICT, AXL_JSON_ERR_DIALECT,
          AXL_JSON_ALLOW_COMMENTS,
          "err: a comment names ALLOW_COMMENTS" },
        { "{'a':1}",         AXL_JSON_STRICT, AXL_JSON_ERR_DIALECT,
          AXL_JSON_ALLOW_SINGLE_QUOTES,
          "err: a single-quoted KEY names ALLOW_SINGLE_QUOTES" },
        { "{\"a\":1,}",      AXL_JSON_STRICT, AXL_JSON_ERR_DIALECT,
          AXL_JSON_ALLOW_TRAILING_COMMA,
          "err: a trailing comma names ALLOW_TRAILING_COMMA "
          "(was 'expected object key (got 0x7D)')" },
        { "{a:1}",           AXL_JSON_STRICT, AXL_JSON_ERR_DIALECT,
          AXL_JSON_ALLOW_UNQUOTED_KEYS,
          "err: an unquoted key names ALLOW_UNQUOTED_KEYS" },
        { "{\"a\":+5}",      AXL_JSON_STRICT, AXL_JSON_ERR_DIALECT,
          AXL_JSON_ALLOW_PLUS_SIGN,
          "err: a leading + names ALLOW_PLUS_SIGN" },
        { "{\"a\":.5}",      AXL_JSON_STRICT, AXL_JSON_ERR_DIALECT,
          AXL_JSON_ALLOW_LEADING_POINT,
          "err: a leading point names ALLOW_LEADING_POINT" },
        { "{\"a\":-NaN}",    AXL_JSON_STRICT, AXL_JSON_ERR_DIALECT,
          AXL_JSON_ALLOW_NAN_INF,
          "err: SIGNED NaN names ALLOW_NAN_INF" },

        // RESTRUCTURED. Each of these reported a generic code before P9,
        // while its sibling above reported DIALECT -- one feature diagnosed
        // two ways depending on a sign, a quote position, or a flag check
        // that ran too early.
        { "{\"a\":NaN}",     AXL_JSON_STRICT, AXL_JSON_ERR_DIALECT,
          AXL_JSON_ALLOW_NAN_INF,
          "err: UNSIGNED NaN names ALLOW_NAN_INF too (was 'unknown literal')" },
        { "{\"a\":'x'}",     AXL_JSON_STRICT, AXL_JSON_ERR_DIALECT,
          AXL_JSON_ALLOW_SINGLE_QUOTES,
          "err: a single-quoted VALUE names ALLOW_SINGLE_QUOTES "
          "(was 'unexpected char')" },
        { "{\"a\":0x1A}",    AXL_JSON_STRICT, AXL_JSON_ERR_DIALECT,
          AXL_JSON_ALLOW_HEX,
          "err: a hex literal names ALLOW_HEX (was a separator error, "
          "reported at the wrong place)" },
        { "{\"a\":\"x\ty\"}", AXL_JSON_STRICT, AXL_JSON_ERR_DIALECT,
          AXL_JSON_ALLOW_EXTRA_WHITESPACE,
          "err: a raw TAB names ALLOW_EXTRA_WHITESPACE, unlike a raw LF" },
    };
    size_t n = sizeof(row) / sizeof(row[0]);
    size_t i;

    for (i = 0; i < n; i++) {
        AxlJsonReader       r;
        const AxlJsonError *e;
        bool                ok;

        ok = axl_json_parse(row[i].doc, axl_strlen(row[i].doc),
                                  row[i].flags, &r);
        e  = axl_json_reader_error(&r);
        test_check(!ok && e->code == row[i].code
                   && e->missing_flag == row[i].flag,
                   row[i].msg);
        if (ok) { axl_json_free(&r); }
    }
}

// ---------------------------------------------------------------------------
// P9 — position, consumed(), and the paths that used to report nothing
// ---------------------------------------------------------------------------

static void
test_json_error_position(void)
{
    AxlJsonReader       r;
    const AxlJsonError *e;

    // --- column counts CHARACTERS, not bytes -----------------------------
    // The ONLY row here that can tell the two apart. On pure ASCII they are
    // equal, so every other position assertion in this file would pass just
    // as well against a byte-counting implementation. The 'é' is two bytes,
    // so a byte column reports 13 where a character column reports 12.
    {
        const char *doc = "{\"\xC3\xA9\":1,\"x\":tru}";

        test_check(!axl_json_parse(doc, axl_strlen(doc),
                                         AXL_JSON_STRICT, &r),
                   "err pos: the multibyte document is rejected");
        e = axl_json_reader_error(&r);
        test_check(e->offset == 12,
                   "err pos: offset is a BYTE index (12)");
        test_check(e->column == 12,
                   "err pos: column is a CHARACTER count (12, not 13)");
        test_check(e->line == 1, "err pos: single-line document is line 1");
    }

    // --- line and column on a multi-line document ------------------------
    {
        const char *doc = "{\n  \"a\": tru\n}";

        test_check(!axl_json_parse(doc, axl_strlen(doc),
                                         AXL_JSON_STRICT, &r),
                   "err pos: the multi-line document is rejected");
        e = axl_json_reader_error(&r);
        test_check(e->line == 2,   "err pos: line counts newlines (2)");
        test_check(e->column == 8, "err pos: column restarts after a newline (8)");
    }

    // --- a SUCCESSFUL parse reports OK and a zeroed position --------------
    {
        const char *doc = "{\"a\":1}";

        test_check(axl_json_parse(doc, axl_strlen(doc),
                                        AXL_JSON_STRICT, &r),
                   "err pos: a good document parses");
        e = axl_json_reader_error(&r);
        test_check(e->code == AXL_JSON_OK && e->offset == 0
                   && e->line == 0 && e->column == 0
                   && e->missing_flag == 0,
                   "err pos: success leaves the error fully zeroed");
        axl_json_free(&r);
    }
}

static void
test_json_reader_consumed(void)
{
    AxlJsonReader r;

    // Trailing whitespace is NOT counted: the next document starts at or
    // after this offset, so a caller that skips whitespace itself does not
    // have it counted twice.
    {
        const char *doc = "{\"a\":1}   ";

        test_check(axl_json_parse(doc, axl_strlen(doc),
                                        AXL_JSON_STRICT, &r)
                   && axl_json_reader_consumed(&r) == 7,
                   "consumed: stops at the root value, excluding trailing space");
        axl_json_free(&r);
    }

    // The point of the accessor: NDJSON. Parse one value, learn where it
    // stopped, parse the next from there. Asserted as an actual round trip
    // rather than as a number, because the number alone would not show that
    // the offset is USABLE as a restart point.
    {
        const char *doc = "{\"a\":1} {\"b\":2}";
        size_t      at;
        int64_t     v = 0;

        test_check(!axl_json_parse(doc, axl_strlen(doc),
                                         AXL_JSON_STRICT, &r)
                   && axl_json_reader_error(&r)->code == AXL_JSON_ERR_TRAILING,
                   "consumed: a second document trailing the first is TRAILING");
        at = axl_json_reader_consumed(&r);
        test_check(at == 8, "consumed: on failure it is where we stopped");

        test_check(axl_json_parse(doc + at, axl_strlen(doc) - at,
                                        AXL_JSON_STRICT, &r)
                   && axl_json_get_int(&r, "b", &v) && v == 2,
                   "consumed: resuming from it parses the NEXT document");
        axl_json_free(&r);
    }

    // A STRING root. The token convention is NOT uniform: [start, end)
    // brackets a string's INNER content, so `end` is the index OF the closing
    // quote, while OBJECT/ARRAY/PRIMITIVE store one-past. Reading `end`
    // uniformly under-reported by one byte -- and only for strings, which is
    // why every other row here passed. Found by review, not by these tests.
    {
        const char *doc = "\"hello\"";

        test_check(axl_json_parse(doc, axl_strlen(doc),
                                        AXL_JSON_STRICT, &r)
                   && axl_json_reader_consumed(&r) == 7,
                   "consumed: a STRING root counts its closing quote");
        axl_json_free(&r);
    }

    // The failure that off-by-one produced, stated as the loop it broke:
    // resuming ON the closing quote reports a spurious INCOMPLETE.
    {
        const char *doc = "\"a\" \"b\"";
        size_t      at;
        char        buf[8];

        test_check(axl_json_parse(doc, axl_strlen(doc),
                                        AXL_JSON_STRICT, &r) == false
                   && axl_json_reader_error(&r)->code == AXL_JSON_ERR_TRAILING,
                   "consumed: two string documents trail");
        at = axl_json_reader_consumed(&r);
        test_check(axl_json_parse(doc + at, axl_strlen(doc) - at,
                                        AXL_JSON_STRICT, &r)
                   && axl_json_value_string(&r, buf, sizeof(buf))
                   && axl_strcmp(buf, "b") == 0,
                   "consumed: an NDJSON loop over STRING documents resumes");
        axl_json_free(&r);
    }

    test_check(axl_json_reader_consumed(NULL) == 0,
               "consumed: NULL reader answers 0");
}

static void
test_json_error_argument_paths(void)
{
    AxlJsonReader r;
    AxlJsonWriter w;

    // These three paths returned false WITHOUT TOUCHING the reader before P9,
    // so a caller following the docstring read stack garbage. Poisoned first
    // so "untouched" cannot masquerade as a pass.
    axl_memset(&r, 0xAA, sizeof(r));
    test_check(!axl_json_parse(NULL, 10, AXL_JSON_STRICT, &r)
               && axl_json_reader_error(&r)->code
                  == AXL_JSON_ERR_INVALID_ARGUMENT,
               "err args: a NULL document reports INVALID_ARGUMENT");

    axl_memset(&r, 0xAA, sizeof(r));
    test_check(!axl_json_parse("{}", 0, AXL_JSON_STRICT, &r)
               && axl_json_reader_error(&r)->code
                  == AXL_JSON_ERR_INVALID_ARGUMENT,
               "err args: a zero length reports INVALID_ARGUMENT");

    {
        AxlJsonReader lr;
        void         *buf = NULL;
        size_t        blen = 0;

        axl_memset(&lr, 0xAA, sizeof(lr));
        test_check(!axl_json_load_file("fs0:\\axl_no_such_file.json", AXL_JSON_RELAXED,
                                       &lr, &buf, &blen)
                   && axl_json_reader_error(&lr)->code == AXL_JSON_ERR_IO,
                   "err args: a file that will not open reports IO");
    }

    test_check(axl_json_reader_error(NULL)->code == AXL_JSON_OK,
               "err args: a NULL reader yields a dereferenceable OK record");

    // --- the writer half --------------------------------------------------
    {
        AXL_AUTOPTR(AxlString) out = axl_string_new(NULL);

        axl_json_writer_init(&w, out, AXL_JSON_STRICT);
        test_check(axl_json_writer_error_info(&w)->code == AXL_JSON_OK,
                   "err args: a freshly initialised writer reads OK");
    }
    axl_memset(&w, 0xAA, sizeof(w));
    axl_json_writer_init(&w, NULL, AXL_JSON_STRICT);
    test_check(axl_json_writer_error_info(&w)->code
               == AXL_JSON_ERR_INVALID_ARGUMENT,
               "err args: a writer with no backing store reports "
               "INVALID_ARGUMENT");
    test_check(axl_json_writer_error_info(NULL)->code == AXL_JSON_OK,
               "err args: a NULL writer yields a dereferenceable OK record");

    // A REAL writer failure, which is the case the accessor exists for and
    // the one it got wrong: 21 sites latched the sticky bool and exactly one
    // set a code, so this reported OK for every genuine error. The pre-existing
    // "misuse sets error" test could not catch it -- it only checked the bool.
    {
        AXL_AUTOPTR(AxlString) out = axl_string_new(NULL);
        const AxlJsonError    *e;

        axl_json_writer_init(&w, out, AXL_JSON_STRICT);
        axl_json_arr_begin(&w);
        axl_json_key(&w, "nope");        /* a key inside an ARRAY */
        e = axl_json_writer_error_info(&w);
        test_check(axl_json_writer_error(&w)
                   && e->code == AXL_JSON_ERR_WRITER_STATE,
                   "err writer: a key inside an array reports WRITER_STATE, "
                   "not OK");
    }
    {
        AXL_AUTOPTR(AxlString) out = axl_string_new(NULL);
        uint32_t               d;

        axl_json_writer_init(&w, out, AXL_JSON_STRICT);
        for (d = 0; d <= AXL_JSON_WRITER_MAX_DEPTH; d++) {
            axl_json_arr_begin(&w);
        }
        test_check(axl_json_writer_error_info(&w)->code == AXL_JSON_ERR_DEPTH,
                   "err writer: nesting past the cap reports DEPTH");
    }
    {
        AXL_AUTOPTR(AxlString) out = axl_string_new(NULL);

        axl_json_writer_init(&w, out, AXL_JSON_STRICT);
        axl_json_obj_begin(&w);
        axl_json_key(&w, NULL);
        test_check(axl_json_writer_error_info(&w)->code
                   == AXL_JSON_ERR_INVALID_ARGUMENT,
                   "err writer: a NULL key reports INVALID_ARGUMENT, "
                   "distinct from state misuse");
    }
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
    test_check(axl_json_load_file(path, AXL_JSON_RELAXED, &r, &raw, &raw_len),
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
    test_check(!axl_json_load_file("fs0:\\__definitely_missing__.tmp", AXL_JSON_RELAXED,
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
    axl_json_writer_init(&w, out, AXL_JSON_STRICT);
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
    test_check(axl_json_parse(built, len, AXL_JSON_RELAXED, &r), "json build: round-trip parse");
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
    axl_json_writer_init(&w, out, AXL_JSON_STRICT);
    axl_json_arr_begin(&w);
    axl_json_key(&w, "bad");        // illegal: key inside array
    axl_json_writer_finish(&w);
    test_check(axl_json_writer_error(&w), "json build: misuse sets error");
    axl_string_free(out);

    // Nested: array of strings, object with named array
    out = axl_string_new(NULL);
    axl_json_writer_init(&w, out, AXL_JSON_STRICT);
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
    axl_json_writer_init(&w, out, AXL_JSON_INDENT(2));
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
    test_check(axl_json_parse(src, axl_strlen(src), AXL_JSON_RELAXED, &r),
               "json bridge: parse src");
    out = axl_string_new(NULL);
    axl_json_writer_init(&w, out, AXL_JSON_STRICT);
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
    test_check(axl_json_parse(uesc, axl_strlen(uesc), AXL_JSON_RELAXED, &r),
               "json bridge: parse escape src");
    out = axl_string_new(NULL);
    axl_json_writer_init(&w, out, AXL_JSON_STRICT);
    axl_json_write_token(&w, &r, 0);
    axl_json_writer_finish(&w);
    test_check(!axl_json_writer_error(&w),
               "json bridge: escape no error");
    test_check(axl_strcmp(axl_string_str(out), uesc) == 0,
               "json bridge: \\uXXXX preserved verbatim");
    axl_json_free(&r);
    axl_string_free(out);

    /* A top-level atom is a DOCUMENT, not a misuse. This assertion was the
       inverse, and its comment said it "matches parser's bare-primitive
       rejection" -- a rule RFC 4627 imposed, RFC 8259 §2 dropped in 2014, and
       P3 removed from the reader. Inverted rather than deleted: the writer
       agreeing with the reader about what a document is only stays true if
       something checks it. Exact string, so this cannot pass on `{...}`. */
    out = axl_string_new(NULL);
    axl_json_writer_init(&w, out, AXL_JSON_STRICT);
    axl_json_str(&w, "lonely");
    axl_json_writer_finish(&w);
    test_check(!axl_json_writer_error(&w) &&
               axl_strcmp(axl_string_str(out), "\"lonely\"") == 0,
               "json build: a top-level atom is a document");
    axl_string_free(out);

    // Empty containers in pretty mode emit no internal whitespace
    out = axl_string_new(NULL);
    axl_json_writer_init(&w, out, AXL_JSON_INDENT(2));
    axl_json_obj_begin(&w);
    axl_json_obj_end(&w);
    axl_json_writer_finish(&w);
    test_check(axl_strcmp(axl_string_str(out), "{}") == 0,
               "json build: pretty empty object is {}");
    axl_string_free(out);

    // UTF-8 passes through byte-for-byte. `char` is SIGNED on both targets, so
    // reading a byte into one puts 0x80-0xFF in -128..-1 — which satisfies the
    // "skip control characters" test and silently ate every non-ASCII byte.
    // RFC 8259 §7 requires escaping only '"', '\\' and 0x00-0x1F, so the raw
    // bytes are correct output. Exact compares, and every quoting path is
    // covered: keys and values, NUL-terminated and counted.
    const char *EM   = "em\xE2\x80\x94""dash";      /* U+2014 */
    const char *CJK  = "\xE4\xB8\xAD\xE6\x96\x87";  /* U+4E2D U+6587 */
    const char *HIGH = "\xC2\x80\xC3\xBF";          /* U+0080, U+00FF — the boundary */

    out = axl_string_new(NULL);
    axl_json_writer_init(&w, out, AXL_JSON_STRICT);
    axl_json_obj_begin(&w);
        axl_json_kv_str(&w, "msg", EM);            /* emit_quoted, value */
        axl_json_key(&w, CJK);                     /* emit_quoted, key   */
        axl_json_str(&w, HIGH);
        axl_json_keyn(&w, CJK, axl_strlen(CJK));   /* emit_quoted_n, key */
        axl_json_strn(&w, EM, axl_strlen(EM));     /* emit_quoted_n, val */
    axl_json_obj_end(&w);
    axl_json_writer_finish(&w);
    test_check(!axl_json_writer_error(&w), "json utf8: no error");
    test_check(axl_strcmp(axl_string_str(out),
                          "{\"msg\":\"em\xE2\x80\x94""dash\","
                          "\"\xE4\xB8\xAD\xE6\x96\x87\":\"\xC2\x80\xC3\xBF\","
                          "\"\xE4\xB8\xAD\xE6\x96\x87\":\"em\xE2\x80\x94""dash\"}") == 0,
               "json utf8: writer emits every non-ASCII byte verbatim");

    // Round-trip it back out, so this pins the whole path a consumer uses
    // (SoftBMC's GET /api/logs was losing the dash between these two points).
    test_check(axl_json_parse(axl_string_str(out),
                              axl_string_len(out), AXL_JSON_RELAXED, &r),
               "json utf8: round-trip parse");
    test_check(axl_json_get_string(&r, "msg", str_buf, sizeof(str_buf))
               && axl_strcmp(str_buf, EM) == 0,
               "json utf8: round-trip preserves the em dash");
    axl_json_free(&r);
    axl_string_free(out);

    // Same defect, third site: the standalone escaper.
    char esc[64];
    test_check(axl_json_escape_string(EM, esc, sizeof(esc)) > 0
               && axl_strcmp(esc, "\"em\xE2\x80\x94""dash\"") == 0,
               "json utf8: escape_string keeps non-ASCII bytes");

    // Control characters are still dropped — the fix must not widen the skip
    // set into a pass-everything.
    test_check(axl_json_escape_string("a\x01\x1F""b", esc, sizeof(esc)) > 0
               && axl_strcmp(esc, "\"ab\"") == 0,
               "json utf8: real control chars are still skipped");

    // ---- Ill-formed UTF-8 in => valid JSON out -----------------------------
    // Letting non-ASCII bytes through (above) means an ill-formed sequence now
    // reaches the output verbatim, and RFC 8259 §8.1 documents are defined over
    // Unicode CODE POINTS — so a lone continuation byte makes the whole document
    // invalid and strict consumers reject it. The writer is the boundary that
    // owes the format's guarantee, so it substitutes U+FFFD.
    //
    // Reachable two ways today, neither hypothetical:
    //   1. axl_log's buf_write truncates at a raw byte count (axl-log.c:55),
    //      so a message crossing MSG_BUF_SIZE mid-sequence leaves a partial one.
    //   2. axl_smbios_get_string_utf8 hands back raw firmware bytes unvalidated;
    //      vendor tables carry latin-1 in the wild.
    //
    // Recovery matches axl_utf8_decode's documented contract exactly — consume
    // ONE byte per ill-formed byte and resynchronize — so the JSON path adds no
    // validator of its own. That yields one U+FFFD per bad byte rather than one
    // per maximal subpart; both are conformant (Unicode's maximal-subpart rule
    // is a recommendation), and the difference only shows on input that is
    // already corrupt.
    // Truncated 3-byte sequence at end of string — the log-truncation shape.
    out = axl_string_new(NULL);
    axl_json_writer_init(&w, out, AXL_JSON_STRICT);
    axl_json_obj_begin(&w);
        axl_json_kv_str(&w, "msg", "em\xE2\x80");
    axl_json_obj_end(&w);
    axl_json_writer_finish(&w);
    test_check(axl_strcmp(axl_string_str(out),
                          "{\"msg\":\"em\xEF\xBF\xBD\xEF\xBF\xBD\"}") == 0,
               "json utf8: truncated sequence becomes U+FFFD");
    axl_string_free(out);

    // Lone continuation byte mid-string, and a lone lead byte.
    out = axl_string_new(NULL);
    axl_json_writer_init(&w, out, AXL_JSON_STRICT);
    axl_json_obj_begin(&w);
        axl_json_kv_str(&w, "cont", "a\x80""b");
        axl_json_kv_str(&w, "over", "\xC0\xAF");        /* overlong '/' */
        axl_json_kv_str(&w, "surr", "\xED\xA0\x80");    /* U+D800 surrogate */
    axl_json_obj_end(&w);
    axl_json_writer_finish(&w);
    test_check(axl_strcmp(axl_string_str(out),
                          "{\"cont\":\"a\xEF\xBF\xBD""b\","
                          "\"over\":\"\xEF\xBF\xBD\xEF\xBF\xBD\","
                          "\"surr\":\"\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD\"}") == 0,
               "json utf8: orphan/overlong/surrogate all become U+FFFD");
    axl_string_free(out);

    // A 4-byte astral sequence is WELL-formed and must survive verbatim — the
    // failure mode of an over-broad fix is mangling valid input.
    out = axl_string_new(NULL);
    axl_json_writer_init(&w, out, AXL_JSON_STRICT);
    axl_json_obj_begin(&w);
        axl_json_kv_str(&w, "emoji", "\xF0\x9F\x98\x80");   /* U+1F600 */
    axl_json_obj_end(&w);
    axl_json_writer_finish(&w);
    test_check(axl_strcmp(axl_string_str(out),
                          "{\"emoji\":\"\xF0\x9F\x98\x80\"}") == 0,
               "json utf8: valid 4-byte astral sequence passes verbatim");
    axl_string_free(out);

    // Counted path: the sequence is cut by n, NOT by a NUL. The bytes past n
    // are present and valid in the underlying buffer, so a writer that decodes
    // without bounding on n emits a well-formed em dash and passes for the
    // wrong reason. Mutating INWARD like this is what makes the assertion
    // discriminate.
    out = axl_string_new(NULL);
    axl_json_writer_init(&w, out, AXL_JSON_STRICT);
    axl_json_obj_begin(&w);
        axl_json_keyn(&w, "em\xE2\x80\x94""k", 4);   /* key   — cuts U+2014 */
        axl_json_strn(&w, EM, 4);                    /* value — cuts U+2014 */
    axl_json_obj_end(&w);
    axl_json_writer_finish(&w);
    test_check(axl_strcmp(axl_string_str(out),
                          "{\"em\xEF\xBF\xBD\xEF\xBF\xBD\":"
                          "\"em\xEF\xBF\xBD\xEF\xBF\xBD\"}") == 0,
               "json utf8: counted path never decodes past n");
    axl_string_free(out);

    // Third quoting path: the standalone escaper.
    test_check(axl_json_escape_string("em\xE2\x80", esc, sizeof(esc)) > 0
               && axl_strcmp(esc, "\"em\xEF\xBF\xBD\xEF\xBF\xBD\"") == 0,
               "json utf8: escape_string substitutes U+FFFD too");

    // The substitute is 3 bytes where the input was 1, so it must respect the
    // caller's buffer and report overflow rather than write past it.
    char tiny[8];
    test_check(axl_json_escape_string("\x80\x80\x80", tiny, sizeof(tiny)) == -1,
               "json utf8: escape_string reports overflow when U+FFFD grows past the buffer");

    // Round-trip: the whole point is that a strict consumer can parse it back.
    out = axl_string_new(NULL);
    axl_json_writer_init(&w, out, AXL_JSON_STRICT);
    axl_json_obj_begin(&w);
        axl_json_kv_str(&w, "msg", "em\xE2\x80");
    axl_json_obj_end(&w);
    axl_json_writer_finish(&w);
    test_check(axl_json_parse(axl_string_str(out), axl_string_len(out), AXL_JSON_RELAXED, &r)
               && axl_json_get_string(&r, "msg", str_buf, sizeof(str_buf))
               && axl_strcmp(str_buf, "em\xEF\xBF\xBD\xEF\xBF\xBD") == 0,
               "json utf8: repaired document round-trips");
    axl_json_free(&r);
    axl_string_free(out);

    // axl_json_write_token splices source bytes verbatim so it preserves the
    // original \uXXXX representation — but the lexer validates no UTF-8 on the
    // way in (that is AXL_JSON_UTF8_* work, still to land on the read side),
    // so a re-serialized document carried ill-formed bytes straight back out.
    // Reached via axl_json_parse, which is AXL_JSON_RELAXED and therefore
    // carries UTF8_RAW — the case this repair exists for. The
    // splice must repair without escaping: every JSON escape byte is ASCII, so
    // "\\u00e9" and friends must survive untouched.
    const char *BAD_DOC = "{\"k\xE2\x80\":\"v\x80\",\"esc\":\"a\\u00e9\\\"b\"}";
    out = axl_string_new(NULL);
    axl_json_writer_init(&w, out, AXL_JSON_STRICT);
    test_check(axl_json_parse(BAD_DOC, axl_strlen(BAD_DOC), AXL_JSON_RELAXED, &r),
               "json utf8: ill-formed source document still parses");
    axl_json_write_token(&w, &r, 0);
    axl_json_writer_finish(&w);
    test_check(axl_strcmp(axl_string_str(out),
                          "{\"k\xEF\xBF\xBD\xEF\xBF\xBD\":\"v\xEF\xBF\xBD\","
                          "\"esc\":\"a\\u00e9\\\"b\"}") == 0,
               "json utf8: write_token repairs key and value, keeps escapes verbatim");
    axl_json_free(&r);
    axl_string_free(out);

    // A comment body is not a "string", but it lands in the same document and
    // an ill-formed byte there invalidates it just the same.
    out = axl_string_new(NULL);
    axl_json_writer_init(&w, out, AXL_JSON_STRICT);
    axl_json_obj_begin(&w);
        axl_json_comment(&w, "note \x80 here");
        axl_json_kv_int(&w, "n", 1);
    axl_json_obj_end(&w);
    axl_json_writer_finish(&w);
    test_check(axl_strcmp(axl_string_str(out),
                          "{/* note \xEF\xBF\xBD here */\"n\":1}") == 0,
               "json utf8: comment body is repaired too");
    axl_string_free(out);

    out = axl_string_new(NULL);
    axl_json_writer_init(&w, out, AXL_JSON_INDENT(2));
    axl_json_arr_begin(&w);
    axl_json_arr_end(&w);
    axl_json_writer_finish(&w);
    test_check(axl_strcmp(axl_string_str(out), "[]") == 0,
               "json build: pretty empty array is []");
    axl_string_free(out);

    // Numeric boundary: INT64_MIN
    out = axl_string_new(NULL);
    axl_json_writer_init(&w, out, AXL_JSON_STRICT);
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
    axl_json_writer_init(&w, out, AXL_JSON_STRICT);
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
    axl_json_writer_init(&w, out, AXL_JSON_STRICT);
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
    axl_json_writer_init(&w, out, AXL_JSON_STRICT);
    axl_json_obj_begin(&w);
    axl_json_key(&w, "x");
    axl_json_raw(&w, NULL);
    test_check(axl_json_writer_error(&w),
               "json build: raw(NULL) sets error");
    axl_string_free(out);

    // axl_json_kv_strn for non-NUL-terminated values
    out = axl_string_new(NULL);
    axl_json_writer_init(&w, out, AXL_JSON_STRICT);
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

    /* --- P1 migration baseline -------------------------------------------
       Pins the EXACT output of every flag the AxlJsonFlags redesign replaces,
       written against the OLD constants and passing BEFORE the rename. The
       clean break is supposed to change names and nothing else, and "nothing
       else" is only a claim until something compares bytes across it.
       Migration table (docs/AXL-JSON-Design.md):
         AXL_JSON_WRITER_DEFAULT         -> AXL_JSON_STRICT
         AXL_JSON_WRITER_PRETTY          -> AXL_JSON_INDENT(2)
         AXL_JSON_WRITER_TRAILING_COMMAS -> AXL_JSON_ALLOW_TRAILING_COMMA
         AXL_JSON_PARSER_DEFAULT         -> AXL_JSON_STRICT
         AXL_JSON_PARSER_JSON5           -> AXL_JSON_JSON5
       The assertions below now use the NEW names; they passed against the OLD
       ones in 60d38100, which is what makes them a before/after proof rather
       than a fresh snapshot of whatever the code does today.
       Each case nests two levels, so indentation depth is pinned rather than
       just "there are newlines". */
    {
        struct { AxlJsonFlags flags; const char *want; const char *what; } mig[] = {
            { AXL_JSON_STRICT,
              "{\"n\":1,\"a\":[2,3],\"o\":{\"k\":\"v\"}}",
              "migration baseline: DEFAULT is compact" },
            { AXL_JSON_INDENT(2),
              "{\n  \"n\": 1,\n  \"a\": [\n    2,\n    3\n  ],\n"
              "  \"o\": {\n    \"k\": \"v\"\n  }\n}",
              "migration baseline: PRETTY is 2-space, nested" },
            { AXL_JSON_ALLOW_TRAILING_COMMA,
              "{\"n\":1,\"a\":[2,3,],\"o\":{\"k\":\"v\",},}",
              "migration baseline: TRAILING_COMMAS at every depth" },
            { AXL_JSON_INDENT(2) | AXL_JSON_ALLOW_TRAILING_COMMA,
              "{\n  \"n\": 1,\n  \"a\": [\n    2,\n    3,\n  ],\n"
              "  \"o\": {\n    \"k\": \"v\",\n  },\n}",
              "migration baseline: PRETTY + TRAILING_COMMAS compose" },
        };
        for (size_t mi = 0; mi < sizeof(mig) / sizeof(mig[0]); mi++) {
            AxlString    *mo = axl_string_new(NULL);
            AxlJsonWriter mw;
            axl_json_writer_init(&mw, mo, mig[mi].flags);
            axl_json_obj_begin(&mw);
                axl_json_kv_int(&mw, "n", 1);
                axl_json_key(&mw, "a");
                axl_json_arr_begin(&mw);
                    axl_json_int(&mw, 2);
                    axl_json_int(&mw, 3);
                axl_json_arr_end(&mw);
                axl_json_key(&mw, "o");
                axl_json_obj_begin(&mw);
                    axl_json_kv_str(&mw, "k", "v");
                axl_json_obj_end(&mw);
            axl_json_obj_end(&mw);
            axl_json_writer_finish(&mw);
            test_check(!axl_json_writer_error(&mw) &&
                       axl_strcmp(axl_string_str(mo), mig[mi].want) == 0,
                       mig[mi].what);
            axl_string_free(mo);
        }

        /* Reader side: the same document under each parser flag. STRICT must
           REJECT the JSON5 spelling, or "JSON5 still works" would pass against
           a parser that had quietly become permissive for everyone. */
        const char *strict_doc = "{\"a\":1}";
        const char *json5_doc  = "{ a: 0x10, /* c */ }";
        AxlJsonReader mr;
        int64_t mv = 0;
        test_check(axl_json_parse(strict_doc, axl_strlen(strict_doc),
                                        AXL_JSON_STRICT, &mr),
                   "migration baseline: PARSER_DEFAULT accepts strict JSON");
        axl_json_free(&mr);
        /* These six were a CHARACTERIZATION of jsmn: compiled without
           JSMN_STRICT it was permissive, so four of them recorded a "strict"
           mode that TOLERATED unquoted keys, hex, single quotes and a trailing
           comma. P3 deleted jsmn and routed every dialect through the one
           lexer, so all four invert -- and that inversion is the phase's
           headline behavior change, which is why they stay here as assertions
           rather than being deleted. The two that already rejected still do.

           Note what did NOT invert: a bare-primitive root. jsmn refused `42`
           and so did the lexer, but RFC 8259 §2 makes any value a document, so
           that shared behavior was a shared BUG. Pinned as accepted below. */
        struct { const char *doc; bool strict_ok; const char *what; } sd[] = {
            { "{ a: 1 }",          false,
              "strict: rejects an unquoted key (was tolerated by jsmn)" },
            { "{ \"a\": 0x10 }",   false,
              "strict: rejects a hex literal (was tolerated by jsmn)" },
            { "{ 'a': 1 }",        false,
              "strict: rejects single quotes (was tolerated by jsmn)" },
            { "{ \"a\": 1, }",     false,
              "strict: rejects a trailing comma (was tolerated by jsmn)" },
            { "// c\n{ \"a\": 1 }", false,
              "strict: rejects a COMMENT (unchanged from jsmn)" },
            { "{ \"a\": 1",         false,
              "strict: rejects an unterminated object (unchanged from jsmn)" },
        };
        for (size_t si = 0; si < sizeof(sd) / sizeof(sd[0]); si++) {
            AxlJsonReader sr;
            bool got = axl_json_parse(sd[si].doc, axl_strlen(sd[si].doc),
                                            AXL_JSON_STRICT, &sr);
            if (got) { axl_json_free(&sr); }
            test_check(got == sd[si].strict_ok, sd[si].what);
        }
        test_check(axl_json_parse(json5_doc, axl_strlen(json5_doc),
                                        AXL_JSON_JSON5, &mr)
                   && axl_json_get_int(&mr, "a", &mv) && mv == 0x10,
                   "migration baseline: PARSER_JSON5 accepts JSON5");
        axl_json_free(&mr);
    }

    /* --- AxlJsonFlags packing ---------------------------------------------
       AXL_JSON_INDENT(n) packs a width into bits 32+ of the same word that
       carries ~20 boolean flags. Two things must hold or the whole space is
       unsafe, and neither is visible from output alone. */
    {
        /* Round-trip over the FULL declared range, not a sample. An off-by-one
           mask (0x1F instead of 0x3F) passes for every n < 32. */
        bool rt_ok = true, presence_ok = true;
        for (uint32_t n = 0; n <= 63; n++) {
            AxlJsonFlags f = AXL_JSON_INDENT(n);
            if (AXL_JSON_INDENT_OF(f) != n)          { rt_ok = false; }
            if ((f & AXL_JSON_HAS_INDENT) == 0)      { presence_ok = false; }
        }
        test_check(rt_ok, "flags: INDENT_OF(INDENT(n)) == n for every n in 0..63");

        /* CLAMPS, does not mask. Masking would turn 64 into 0 -- a wider
           request silently becoming "no indent at all" -- which is the wrong
           answer a caller computing a width from config would get. */
        test_check(AXL_JSON_INDENT_OF(AXL_JSON_INDENT(64)) == 63 &&
                   AXL_JSON_INDENT_OF(AXL_JSON_INDENT(1000)) == 63,
                   "flags: INDENT(n) clamps above the max instead of wrapping to 0");
        /* The macro must stay a CONSTANT EXPRESSION -- a file-scope
           initializer proves it, and nothing else in the suite would (C99
           lets automatic aggregates take non-constant initializers, so every
           in-function use compiles either way). */
        test_check(AXL_JSON_INDENT_OF(kIndentConstExpr) == 2 &&
                   (kIndentConstExpr & AXL_JSON_COMPACT) != 0,
                   "flags: INDENT(n) is usable in a constant expression");
        /* The single-evaluation form, for a runtime/side-effecting width. */
        uint32_t side = 3;
        AxlJsonFlags once = axl_json_indent(side++);
        test_check(side == 4 && AXL_JSON_INDENT_OF(once) == 3,
                   "flags: axl_json_indent evaluates n exactly once");
        test_check(axl_json_indent(1000) == AXL_JSON_INDENT(1000),
                   "flags: macro and function forms agree, both clamped");
        test_check(presence_ok, "flags: INDENT(n) always sets the presence bit");

        /* The presence bit is the ONLY thing separating "no indent" from
           INDENT(0) -- both have a zero width field. If these ever compare
           equal, compact and newlines-at-zero-indent become indistinguishable. */
        test_check(AXL_JSON_INDENT(0) != AXL_JSON_STRICT,
                   "flags: INDENT(0) is distinguishable from no-indent");
        test_check(AXL_JSON_INDENT_OF(AXL_JSON_STRICT) == 0,
                   "flags: INDENT_OF is 0 when no indent was requested");

        /* No collision: the widest indent OR'd with every boolean flag we
           define. Each must still read back, and the width must survive. */
        const AxlJsonFlags all_bools =
            AXL_JSON_JSON5 | AXL_JSON_COMPACT | AXL_JSON_ENSURE_ASCII |
            AXL_JSON_ESCAPE_SLASH | AXL_JSON_EMBED | AXL_JSON_SORT_KEYS |
            AXL_JSON_REJECT_DUPLICATES |
            AXL_JSON_EXTENDED | AXL_JSON_UTF8_STRICT;
        const AxlJsonFlags mixed = all_bools | AXL_JSON_INDENT(63);
        test_check(AXL_JSON_INDENT_OF(mixed) == 63,
                   "flags: a full boolean set does not disturb the indent width");

        /* TWO packed fields now share bits 32+. Neither may bleed into the
           other, and neither into the booleans -- an off-by-one shift or a
           too-wide mask is invisible until a caller combines them. */
        const AxlJsonFlags both_packed =
            all_bools | AXL_JSON_INDENT(63) | AXL_JSON_DEPTH(AXL_JSON_DEPTH_MAX);
        test_check(AXL_JSON_INDENT_OF(both_packed) == 63 &&
                   AXL_JSON_DEPTH_OF(both_packed) == AXL_JSON_DEPTH_MAX,
                   "flags: indent and depth fields read back independently");
        test_check(AXL_JSON_DEPTH_OF(AXL_JSON_INDENT(63)) == 0,
                   "flags: a maximal indent leaves the depth field clear");
        test_check(AXL_JSON_INDENT_OF(AXL_JSON_DEPTH(AXL_JSON_DEPTH_MAX)) == 0,
                   "flags: a maximal depth leaves the indent field clear");
        test_check((AXL_JSON_DEPTH(AXL_JSON_DEPTH_MAX) & 0xFFFFFFFFu) == 0,
                   "flags: the depth field is clear of every boolean bit");

        /* Round-trip over the whole declared range, not a sample: an 0x1FF
           mask instead of 0x3FF reads 256 back as 0, i.e. "use the default",
           which is a silently WRONG limit rather than a visible failure. */
        bool depth_rt = true;
        for (uint32_t n = 0; n <= AXL_JSON_DEPTH_MAX; n++) {
            if (AXL_JSON_DEPTH_OF(AXL_JSON_DEPTH(n)) != n) { depth_rt = false; }
        }
        test_check(depth_rt,
                   "flags: DEPTH_OF(DEPTH(n)) == n for every n in 0..MAX");
        test_check(AXL_JSON_DEPTH_OF(AXL_JSON_DEPTH(AXL_JSON_DEPTH_MAX + 1))
                       == AXL_JSON_DEPTH_MAX &&
                   AXL_JSON_DEPTH_OF(AXL_JSON_DEPTH(100000)) == AXL_JSON_DEPTH_MAX,
                   "flags: DEPTH(n) clamps above the max instead of wrapping");
        test_check(AXL_JSON_DEPTH_OF(AXL_JSON_STRICT) == 0,
                   "flags: DEPTH_OF is 0 (meaning default) when none requested");
        uint32_t dside = 7;
        AxlJsonFlags donce = axl_json_depth(dside++);
        test_check(dside == 8 && AXL_JSON_DEPTH_OF(donce) == 7,
                   "flags: axl_json_depth evaluates n exactly once");
        test_check(axl_json_depth(100000) == AXL_JSON_DEPTH(100000),
                   "flags: DEPTH macro and function forms agree, both clamped");

        /* DISJOINTNESS, one flag at a time. The obvious spelling --
           `(all_bools | INDENT(63)) & all_bools == all_bools` -- is a
           tautology: (A|B)&A == A for ANY A and B, so it passes even against a
           flag space with duplicate bits. Accumulating and checking for an
           already-present bit is what actually detects a collision. */
        const AxlJsonFlags each[] = {
            AXL_JSON_ALLOW_COMMENTS, AXL_JSON_ALLOW_TRAILING_COMMA,
            AXL_JSON_ALLOW_UNQUOTED_KEYS, AXL_JSON_ALLOW_SINGLE_QUOTES,
            AXL_JSON_ALLOW_HEX, AXL_JSON_ALLOW_EXTRA_ESCAPES,
            AXL_JSON_ALLOW_PLUS_SIGN, AXL_JSON_ALLOW_LEADING_POINT,
            AXL_JSON_ALLOW_NAN_INF,
            AXL_JSON_COMPACT, AXL_JSON_ENSURE_ASCII, AXL_JSON_ESCAPE_SLASH,
            AXL_JSON_EMBED, AXL_JSON_SORT_KEYS, AXL_JSON_HAS_INDENT,
            AXL_JSON_REJECT_DUPLICATES,
            AXL_JSON_EXTENDED,
        };
        AxlJsonFlags seen = 0;
        bool disjoint = true;
        for (size_t fi = 0; fi < sizeof(each) / sizeof(each[0]); fi++) {
            if ((seen & each[fi]) != 0) { disjoint = false; }
            seen |= each[fi];
        }
        test_check(disjoint, "flags: every boolean flag occupies its own bit");
        test_check((seen & AXL_JSON_UTF8_MASK) == 0,
                   "flags: no boolean flag lands in the UTF-8 field");
        test_check(AXL_JSON_INDENT_OF(seen) == 0,
                   "flags: no boolean flag lands in the indent field");
        test_check(AXL_JSON_DEPTH_OF(seen) == 0,
                   "flags: no boolean flag lands in the depth field");

        /* The dialect bits must stay disjoint from every non-dialect flag.
           P3 removed the routing this once protected -- every document now
           goes to the one lexer -- but the property still matters: the lexer
           tests `flags & AXL_JSON_ALLOW_x` per feature, so a non-dialect flag
           overlapping bits 0-9 would silently open a grammar extension. */
        test_check((AXL_JSON_JSON5 & (AXL_JSON_COMPACT | AXL_JSON_ENSURE_ASCII |
                                      AXL_JSON_ESCAPE_SLASH | AXL_JSON_EMBED |
                                      AXL_JSON_SORT_KEYS | AXL_JSON_HAS_INDENT |
                                      AXL_JSON_REJECT_DUPLICATES |
                                      AXL_JSON_EXTENDED |
                                      AXL_JSON_UTF8_MASK)) == 0,
                   "flags: the dialect mask is disjoint from every non-dialect flag");

        /* The UTF-8 field is a FIELD, so the three modes must be mutually
           exclusive values -- not bits that can both be set. */
        test_check(AXL_JSON_UTF8_OF(AXL_JSON_UTF8_REPAIR) == AXL_JSON_UTF8_REPAIR &&
                   AXL_JSON_UTF8_OF(mixed) == AXL_JSON_UTF8_STRICT,
                   "flags: UTF8_OF extracts the mode, masking the rest away");
        test_check(AXL_JSON_UTF8_RAW != AXL_JSON_UTF8_STRICT &&
                   (AXL_JSON_UTF8_RAW & AXL_JSON_UTF8_STRICT) != AXL_JSON_UTF8_STRICT,
                   "flags: RAW and STRICT are distinct field VALUES, not combinable bits");

        /* Presets compose as documented. */
        test_check((AXL_JSON_RELAXED & AXL_JSON_JSON5) == AXL_JSON_JSON5,
                   "flags: RELAXED includes all of JSON5");
        test_check(AXL_JSON_STRICT == 0,
                   "flags: STRICT is the zero word");
    }

    /* INDENT(n) must honor n. Widths other than 2 on purpose: the migration
       baseline uses INDENT(2), which is exactly the width at which a
       hardcoded two-space indent is indistinguishable from a correct one.
       Also pins INDENT(0) -- newlines, no indent -- which is the case the
       presence bit exists to separate from "compact". */
    {
        struct { AxlJsonFlags f; const char *want; const char *what; } ind[] = {
            { AXL_JSON_INDENT(0),
              "{\n\"a\": [\n1\n]\n}",
              "indent: INDENT(0) is newlines with no indent" },
            { AXL_JSON_INDENT(1),
              "{\n \"a\": [\n  1\n ]\n}",
              "indent: INDENT(1) is one space per level" },
            { AXL_JSON_INDENT(4),
              "{\n    \"a\": [\n        1\n    ]\n}",
              "indent: INDENT(4) is four spaces per level" },
        };
        for (size_t ii = 0; ii < sizeof(ind) / sizeof(ind[0]); ii++) {
            AxlString    *io = axl_string_new(NULL);
            AxlJsonWriter iw;
            axl_json_writer_init(&iw, io, ind[ii].f);
            axl_json_obj_begin(&iw);
                axl_json_key(&iw, "a");
                axl_json_arr_begin(&iw);
                    axl_json_int(&iw, 1);
                axl_json_arr_end(&iw);
            axl_json_obj_end(&iw);
            axl_json_writer_finish(&iw);
            test_check(axl_strcmp(axl_string_str(io), ind[ii].want) == 0,
                       ind[ii].what);
            axl_string_free(io);
        }
    }

    /* --- P2: the sub-flag REJECTION MATRIX --------------------------------
       The load-bearing test of the whole granular design, and the reason it
       is a matrix rather than a list.

       The obvious test -- "flag X accepts feature X" -- is worthless on its
       own: the lexer used to be monolithic, so an implementation that ignores
       the flag word entirely and accepts all of JSON5 passes every positive
       case. What discriminates is the NEGATIVE half: flag X must REJECT
       features Y != X. Verified by sabotage before landing — with the gates
       stubbed to `true`, the diagonal still passes and 56 off-diagonal cells
       fail.

       ALLOW_NAN_INF is absent from the matrix on purpose, even though P4 made
       it gate something: its feature is a bare WORD, not punctuation, so a row
       here would test the keyword table rather than the number lexer the other
       eight share. It gets its own block below, with both endpoints. */
    {
        struct { AxlJsonFlags flag; const char *doc; const char *name; } feat[] = {
            { AXL_JSON_ALLOW_COMMENTS,       "{\"a\":1 /* c */}",  "comments" },
            { AXL_JSON_ALLOW_TRAILING_COMMA, "{\"a\":1,}",         "trailing-comma" },
            { AXL_JSON_ALLOW_UNQUOTED_KEYS,  "{a:1}",              "unquoted-key" },
            { AXL_JSON_ALLOW_SINGLE_QUOTES,  "{\"a\":'v'}",        "single-quotes" },
            { AXL_JSON_ALLOW_HEX,            "{\"a\":0x10}",       "hex" },
            { AXL_JSON_ALLOW_EXTRA_ESCAPES,  "{\"a\":\"\\x41\"}",   "extra-escapes" },
            { AXL_JSON_ALLOW_PLUS_SIGN,      "{\"a\":+5}",         "plus-sign" },
            { AXL_JSON_ALLOW_LEADING_POINT,  "{\"a\":.5}",         "leading-point" },
        };
        const size_t nf = sizeof(feat) / sizeof(feat[0]);

        size_t diag_ok = 0, offdiag_ok = 0, diag_bad = 0, offdiag_bad = 0;
        for (size_t row = 0; row < nf; row++) {
            for (size_t col = 0; col < nf; col++) {
                AxlJsonReader mr;
                bool got = axl_json_parse(feat[col].doc,
                                                axl_strlen(feat[col].doc),
                                                feat[row].flag, &mr);
                if (got) { axl_json_free(&mr); }
                const bool want = (row == col);
                if (want) { got ? diag_ok++ : diag_bad++; }
                else      { got ? offdiag_bad++ : offdiag_ok++; }
            }
        }
        test_check(diag_bad == 0 && diag_ok == nf,
                   "matrix: each ALLOW_* flag accepts its OWN feature");
        test_check(offdiag_bad == 0 && offdiag_ok == nf * (nf - 1),
                   "matrix: each ALLOW_* flag REJECTS every other feature");

        /* Per row as well as in aggregate. The two assertions above collapse
           56 cells into one boolean: when a cell breaks you learn only that
           SOMETHING moved and get to re-derive which. One check per row names
           the flag, which is why feat[].name exists. */
        for (size_t row = 0; row < nf; row++) {
            size_t rejected = 0;
            for (size_t col = 0; col < nf; col++) {
                if (row == col) { continue; }
                AxlJsonReader mr;
                bool got = axl_json_parse(feat[col].doc,
                                                axl_strlen(feat[col].doc),
                                                feat[row].flag, &mr);
                if (got) { axl_json_free(&mr); } else { rejected++; }
            }
            char label[96];
            axl_snprintf(label, sizeof(label),
                         "matrix row: %s rejects the other %zu features",
                         feat[row].name, nf - 1);
            test_check(rejected == nf - 1, label);
        }

        /* Two bits at once, to pin that the gates compose independently
           rather than sharing a latch: both named features accepted, the
           other six still refused. */
        {
            size_t both_ok = 0, others_rejected = 0;
            const AxlJsonFlags two = AXL_JSON_ALLOW_COMMENTS | AXL_JSON_ALLOW_HEX;
            for (size_t i = 0; i < nf; i++) {
                AxlJsonReader mr;
                bool got = axl_json_parse(feat[i].doc,
                                                axl_strlen(feat[i].doc), two, &mr);
                if (got) { axl_json_free(&mr); }
                const bool named = (feat[i].flag == AXL_JSON_ALLOW_COMMENTS ||
                                    feat[i].flag == AXL_JSON_ALLOW_HEX);
                if (named && got)        { both_ok++; }
                if (!named && !got)      { others_rejected++; }
            }
            test_check(both_ok == 2 && others_rejected == nf - 2,
                       "matrix: two flags compose — both accepted, the rest refused");
        }

        /* Endpoints. STRICT must reject all eight -- that is the property the
           old jsmn-backed "strict" mode did NOT have. */
        size_t strict_rejected = 0, json5_accepted = 0;
        for (size_t i = 0; i < nf; i++) {
            AxlJsonReader sr, jr;
            if (!axl_json_parse(feat[i].doc, axl_strlen(feat[i].doc),
                                      AXL_JSON_STRICT, &sr)) {
                strict_rejected++;
            } else { axl_json_free(&sr); }
            if (axl_json_parse(feat[i].doc, axl_strlen(feat[i].doc),
                                     AXL_JSON_JSON5, &jr)) {
                json5_accepted++;
                axl_json_free(&jr);
            }
        }
        test_check(json5_accepted == nf,
                   "matrix: AXL_JSON_JSON5 accepts every feature");

        /* THE proof that P3 did what it claims. This read `== 1` for as long
           as AXL_JSON_STRICT routed to jsmn: compiled permissively, jsmn
           refused exactly one of the eight (the \x escape) and tolerated an
           interior comment, a trailing comma, an unquoted key, single quotes,
           hex, a leading '+' and a leading '.'. One parser later it is all
           eight. Pinned as the exact number, not `> 1` or `>= nf` -- a bound
           would keep passing if a future change quietly re-opened one. */
        test_check(strict_rejected == nf,
                   "matrix: AXL_JSON_STRICT refuses all 8 (one parser, no jsmn)");

        /* ALLOW_NAN_INF is now the ninth gated feature, so it joins the
           endpoints: JSON5 accepts it, STRICT refuses it. This assertion used
           to read `!nan_ok` under JSON5 -- the RED P4 was written against. */
        const char *nan_doc = "{\"a\":NaN}";
        AxlJsonReader nr;
        bool nan_ok = axl_json_parse(nan_doc, axl_strlen(nan_doc),
                                           AXL_JSON_JSON5, &nr);
        if (nan_ok) { axl_json_free(&nr); }
        test_check(nan_ok, "matrix: NaN is accepted under JSON5");
        bool nan_strict = axl_json_parse(nan_doc, axl_strlen(nan_doc),
                                               AXL_JSON_STRICT, &nr);
        if (nan_strict) { axl_json_free(&nr); }
        test_check(!nan_strict, "matrix: NaN is refused under STRICT");
    }

    /* --- P4: ALLOW_NAN_INF, and get_number_str as the way to read one -------
       Not IEEE support. AXL is freestanding with no libm and no double
       accessor, so NaN/Infinity are lexed as primitive TOKENS and are
       reachable only as text. get_int/get_uint must refuse them -- there is no
       integer they could mean -- which is exactly why the text accessor has to
       exist alongside the flag rather than after it. */
    {
        /* Each spelling, and which flags it needs. `+Infinity` needs
           ALLOW_PLUS_SIGN too: the leading `+` is that flag's feature, so
           ALLOW_NAN_INF alone must NOT be enough. That pair is the
           discriminating case -- a lexer that folded the sign into NAN_INF
           passes every other row here. */
        struct { const char *doc; AxlJsonFlags f; bool ok; const char *what; } ni[] = {
            { "{\"a\":NaN}",        AXL_JSON_ALLOW_NAN_INF, true,
              "nan_inf: NaN with the flag" },
            { "{\"a\":Infinity}",   AXL_JSON_ALLOW_NAN_INF, true,
              "nan_inf: Infinity with the flag" },
            { "{\"a\":-Infinity}",  AXL_JSON_ALLOW_NAN_INF, true,
              "nan_inf: -Infinity with the flag ('-' is RFC-legal on numbers)" },
            { "{\"a\":-NaN}",       AXL_JSON_ALLOW_NAN_INF, true,
              "nan_inf: -NaN with the flag (ES5 permits a sign on either word)" },
            { "{\"a\":NaN}",        AXL_JSON_STRICT,        false,
              "nan_inf: NaN without the flag" },
            { "{\"a\":Infinity}",   AXL_JSON_STRICT,        false,
              "nan_inf: Infinity without the flag" },
            { "{\"a\":-Infinity}",  AXL_JSON_STRICT,        false,
              "nan_inf: -Infinity without the flag" },
            { "{\"a\":+Infinity}",  AXL_JSON_ALLOW_NAN_INF, false,
              "nan_inf: +Infinity needs ALLOW_PLUS_SIGN as well" },
            { "{\"a\":+Infinity}",
              AXL_JSON_ALLOW_NAN_INF | AXL_JSON_ALLOW_PLUS_SIGN, true,
              "nan_inf: +Infinity with both flags" },
            /* Not a licence for any bare word: only these two. */
            { "{\"a\":Inf}",        AXL_JSON_ALLOW_NAN_INF, false,
              "nan_inf: `Inf` is not a literal even with the flag" },
            { "{\"a\":nan}",        AXL_JSON_ALLOW_NAN_INF, false,
              "nan_inf: lowercase `nan` is not a literal (case-sensitive)" },
            { "{\"a\":NaNa}",       AXL_JSON_ALLOW_NAN_INF, false,
              "nan_inf: `NaNa` is rejected, not lexed as NaN + junk" },
            { "{\"a\":Infinity2}",  AXL_JSON_ALLOW_NAN_INF, false,
              "nan_inf: `Infinity2` is rejected, not lexed as Infinity + junk" },
        };
        for (size_t i = 0; i < sizeof(ni) / sizeof(ni[0]); i++) {
            AxlJsonReader r2;
            bool got = axl_json_parse(ni[i].doc, axl_strlen(ni[i].doc),
                                            ni[i].f, &r2);
            if (got) { axl_json_free(&r2); }
            test_check(got == ni[i].ok, ni[i].what);
        }

        /* get_int / get_uint must refuse a NaN/Infinity token. Without this,
           "lexed as a token" could quietly mean get_int returns 0. */
        {
            const char *d = "{\"n\":NaN,\"i\":-Infinity}";
            AxlJsonReader r2;
            int64_t  iv = 12345;
            uint64_t uv = 54321;
            test_check(axl_json_parse(d, axl_strlen(d),
                                            AXL_JSON_ALLOW_NAN_INF, &r2),
                       "nan_inf: document with NaN and -Infinity parses");
            test_check(!axl_json_get_int(&r2, "n", &iv) && iv == 12345,
                       "nan_inf: get_int refuses NaN and leaves the out param");
            test_check(!axl_json_get_uint(&r2, "n", &uv) && uv == 54321,
                       "nan_inf: get_uint refuses NaN and leaves the out param");
            test_check(!axl_json_get_int(&r2, "i", &iv) && iv == 12345,
                       "nan_inf: get_int refuses -Infinity");
            /* And they ARE reachable, as text, exactly as spelled. */
            char nb[16] = { 0 };
            test_check(axl_json_get_number_str(&r2, "n", nb, sizeof(nb)) &&
                       axl_strcmp(nb, "NaN") == 0,
                       "number_str: NaN comes back as \"NaN\"");
            test_check(axl_json_get_number_str(&r2, "i", nb, sizeof(nb)) &&
                       axl_strcmp(nb, "-Infinity") == 0,
                       "number_str: -Infinity comes back as \"-Infinity\"");
            axl_json_free(&r2);
        }

        /* VERBATIM: the text must be the document's bytes, not a normalization
           of them -- `1e10` must NOT come back as "10000000000".
           Most of these are values get_int and get_uint both have to refuse
           (wider than 64 bits, fractional, exponent-bearing). Not all: `d20`
           (12345678901234567890) fits uint64_t and only get_int refuses it, and
           `hex`/`plus`/`lead` are perfectly readable as integers. They are here
           for the SPELLING, which is the property under test. */
        {
            const char *d =
                "{\"big\":18446744073709551616,"          /* 2^64 */
                "\"huge\":1180591620717411303424,"        /* 2^70 */
                "\"d20\":12345678901234567890,"
                "\"frac\":1.5,"
                "\"exp\":1e10,"
                "\"tz\":0.50,"
                "\"negexp\":-2.5E-3,"
                "\"plus\":+5,"
                "\"hex\":0x1F,"
                "\"lead\":.5}";
            struct { const char *key; const char *want; } v[] = {
                { "big",    "18446744073709551616" },
                { "huge",   "1180591620717411303424" },
                { "d20",    "12345678901234567890" },
                { "frac",   "1.5" },
                { "exp",    "1e10" },
                { "tz",     "0.50" },
                { "negexp", "-2.5E-3" },
                { "plus",   "+5" },
                { "hex",    "0x1F" },
                { "lead",   ".5" },
            };
            AxlJsonReader r2;
            test_check(axl_json_parse(d, axl_strlen(d),
                                            AXL_JSON_JSON5, &r2),
                       "number_str: the wide/fractional/JSON5 document parses");
            size_t exact = 0;
            for (size_t i = 0; i < sizeof(v) / sizeof(v[0]); i++) {
                char buf[40] = { 0 };
                if (axl_json_get_number_str(&r2, v[i].key, buf, sizeof(buf)) &&
                    axl_strcmp(buf, v[i].want) == 0) {
                    exact++;
                } else {
                    axl_printf("  number_str MISMATCH %s: got \"%s\" want \"%s\"\n",
                               v[i].key, buf, v[i].want);
                }
            }
            test_check(exact == sizeof(v) / sizeof(v[0]),
                       "number_str: every literal comes back byte-for-byte");

            /* The values get_uint CAN represent are still its job -- this
               accessor is an escape hatch, not a replacement. */
            uint64_t uv = 0;
            test_check(!axl_json_get_uint(&r2, "big", &uv),
                       "number_str: get_uint still refuses 2^64 (the reason this exists)");

            /* Buffer sizing. "1.5" needs 4 bytes with the NUL; 4 must work and
               3 must fail, and on failure the buffer is UNTOUCHED -- a partial
               number is a different number, so truncating like get_string does
               would defeat the whole point. */
            char tight[4] = { 0 };
            test_check(axl_json_get_number_str(&r2, "frac", tight, 4) &&
                       axl_strcmp(tight, "1.5") == 0,
                       "number_str: a buffer exactly big enough succeeds");
            char small[8];
            axl_memset(small, 'Z', sizeof(small));
            test_check(!axl_json_get_number_str(&r2, "frac", small, 3),
                       "number_str: one byte too small returns false");
            test_check(small[0] == 'Z' && small[1] == 'Z' && small[2] == 'Z',
                       "number_str: a too-small buffer is left UNTOUCHED, not truncated");

            /* Not a number: true/false/null are primitive tokens too, and an
               accessor named _number_str returning "true" would be a trap. */
            axl_json_free(&r2);
            const char *d2 = "{\"t\":true,\"n\":null,\"s\":\"12\",\"o\":{},\"a\":[]}";
            test_check(axl_json_parse(d2, axl_strlen(d2),
                                            AXL_JSON_STRICT, &r2),
                       "number_str: the non-number document parses");
            char nb[16];
            size_t refused = 0;
            const char *keys[] = { "t", "n", "s", "o", "a", "missing" };
            for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
                axl_memset(nb, 'Z', sizeof(nb));
                if (!axl_json_get_number_str(&r2, keys[i], nb, sizeof(nb))
                    && nb[0] == 'Z') {
                    refused++;
                }
            }
            test_check(refused == sizeof(keys) / sizeof(keys[0]),
                       "number_str: refuses true/null/string/object/array/absent");

            /* NULL-argument contract, matching every other accessor. */
            test_check(!axl_json_get_number_str(NULL, "frac", nb, sizeof(nb)) &&
                       !axl_json_get_number_str(&r2, NULL, nb, sizeof(nb)) &&
                       !axl_json_get_number_str(&r2, "frac", NULL, sizeof(nb)) &&
                       !axl_json_get_number_str(&r2, "frac", nb, 0),
                       "number_str: NULL reader/key/buf and size 0 all return false");
            axl_json_free(&r2);
        }

        /* Through a SUB-READER, from both get_object and array_next. A
           sub-reader borrows the parent's token array rebased, so an accessor
           that got the rebasing wrong would read a neighbouring token -- and
           "correct by inspection" is how that stays unnoticed. */
        {
            const char *d = "{\"o\":{\"v\":1e99},\"a\":[{\"v\":-Infinity}]}";
            AxlJsonReader r2, sub, elem;
            AxlJsonArrayIter it;
            char nb[24] = { 0 };
            test_check(axl_json_parse(d, axl_strlen(d),
                                            AXL_JSON_JSON5, &r2),
                       "number_str: nested document parses");
            test_check(axl_json_get_object(&r2, "o", &sub) &&
                       axl_json_get_number_str(&sub, "v", nb, sizeof(nb)) &&
                       axl_strcmp(nb, "1e99") == 0,
                       "number_str: reads through a get_object sub-reader");
            nb[0] = '\0';
            test_check(axl_json_array_begin(&r2, "a", &it) &&
                       axl_json_array_next(&it, &elem) &&
                       axl_json_get_number_str(&elem, "v", nb, sizeof(nb)) &&
                       axl_strcmp(nb, "-Infinity") == 0,
                       "number_str: reads through an array element sub-reader");
            axl_json_free(&r2);
        }

        /* `NaN` and `Infinity` are VALUE literals, not a new key syntax. An
           unquoted key goes through the identifier lexer, so the words must
           still need ALLOW_UNQUOTED_KEYS and must read back as plain key text.
           Unpinned, a future keyword-table change could quietly special-case
           them in key position. */
        {
            const char *d = "{NaN:1,Infinity:2}";
            AxlJsonReader r2;
            int64_t v = 0;
            test_check(!axl_json_parse(d, axl_strlen(d),
                                             AXL_JSON_ALLOW_NAN_INF, &r2),
                       "nan_inf: NaN as an unquoted KEY still needs ALLOW_UNQUOTED_KEYS");
            test_check(axl_json_parse(d, axl_strlen(d),
                                            AXL_JSON_ALLOW_NAN_INF |
                                            AXL_JSON_ALLOW_UNQUOTED_KEYS, &r2),
                       "nan_inf: NaN as an unquoted key parses with that flag");
            test_check(axl_json_get_int(&r2, "NaN", &v) && v == 1,
                       "nan_inf: a key spelled NaN is ordinary key text");
            test_check(axl_json_get_int(&r2, "Infinity", &v) && v == 2,
                       "nan_inf: a key spelled Infinity is ordinary key text");
            axl_json_free(&r2);
        }
    }

    /* --- P3: a bare primitive IS a document (RFC 8259 §2) -----------------
       Both parsers used to require an object-or-array root, a rule RFC 4627
       imposed and RFC 8259 (2014) dropped. It was the ONLY thing making
       AXL_JSON_STRICT reject a conformance case -- 8 of JSONTestSuite's `y_`
       files are bare roots -- so "strict" was stricter than the standard it
       names. AXL_JSON_DECODE_ANY existed to unlock this and is now removed:
       there is nothing left to unlock. */
    {
        struct { const char *doc; const char *what; } root[] = {
            { "42",      "bare int" },
            { "-0.5e3",  "bare real" },
            { "true",    "bare true" },
            { "false",   "bare false" },
            { "null",    "bare null" },
            { "\"asd\"", "bare string" },
            { "\"\"",    "bare empty string" },
            { " \"a\" ", "bare string with surrounding space" },
        };
        size_t accepted = 0;
        for (size_t ri = 0; ri < sizeof(root) / sizeof(root[0]); ri++) {
            AxlJsonReader rr;
            if (axl_json_parse(root[ri].doc, axl_strlen(root[ri].doc),
                                     AXL_JSON_STRICT, &rr)) {
                accepted++;
                axl_json_free(&rr);
            } else {
                axl_printf("  bare root REJECTED: %s\n", root[ri].what);
            }
        }
        test_check(accepted == sizeof(root) / sizeof(root[0]),
                   "root: STRICT accepts every bare-primitive document");

        /* Readable, not merely parseable. axl_json_value_string is the only
           accessor that can reach a bare-string root, and its docstring now
           promises exactly that -- so the promise gets an assertion. */
        AxlJsonReader sr;
        char sv[16] = { 0 };
        test_check(axl_json_parse("\"hi\"", 4, AXL_JSON_STRICT, &sr) &&
                   axl_json_value_string(&sr, sv, sizeof(sv)) &&
                   axl_strcmp(sv, "hi") == 0,
                   "root: a bare-string document reads back through value_string");
        axl_json_free(&sr);

        /* Accepting a bare root must not make the object accessors lie about
           one: a key lookup against `42` has no answer and must say so. */
        AxlJsonReader ir;
        int64_t iv = 0;
        test_check(axl_json_parse("42", 2, AXL_JSON_STRICT, &ir),
                   "root: bare int parses");
        test_check(!axl_json_get_int(&ir, "a", &iv),
                   "root: get_int on a bare-root document finds no key");
        test_check(!axl_json_value_array_begin(&ir, &(AxlJsonArrayIter){ 0 }),
                   "root: value_array_begin refuses a bare-primitive root");
        axl_json_free(&ir);

        /* Still rejected: garbage after a bare root, and a truncated literal.
           A root relaxation must not become "parse a prefix and stop". */
        struct { const char *doc; const char *what; } bad[] = {
            { "42 43",  "root: two bare values are not one document" },
            { "truex",  "root: a truncated literal is still rejected" },
            { "nul",    "root: an incomplete null is still rejected" },
            { "42abc",  "root: trailing garbage after a bare number is rejected" },
        };
        for (size_t bi = 0; bi < sizeof(bad) / sizeof(bad[0]); bi++) {
            AxlJsonReader br;
            bool got = axl_json_parse(bad[bi].doc, axl_strlen(bad[bi].doc),
                                            AXL_JSON_STRICT, &br);
            if (got) { axl_json_free(&br); }
            /* Per case, not a running total. Asserting a cumulative counter
               against `bi + 1` reports a FAILURE for every case AFTER the
               first real one, because the counter stays permanently one
               behind -- so a maintainer reads three extra failures that are
               not failures, and the label no longer names the broken case. */
            test_check(!got, bad[bi].what);
        }
    }

    /* --- P3: the WRITER emits a bare-primitive root too -------------------
       Reader and writer have to agree on what a document is, which is the
       whole point of the shared flag space. check_atom_context used to refuse
       any atom at depth 0, justified in a comment by "mirror axl_json_parse,
       which requires the root token to be an object or array" -- the rule P3
       deletes. Leaving the writer alone would have meant a reader that parses
       `42` and a writer that cannot emit it, with a comment citing a rule that
       no longer exists. EXACT whole-document compares, per the output-format
       rule: a substring match would pass on `{"a":42}` too. */
    {
        struct { const char *want; int kind; const char *what; } wr[] = {
            { "42",     0, "writer: a bare int is a document" },
            { "\"hi\"", 1, "writer: a bare string is a document" },
            { "true",   2, "writer: a bare true is a document" },
            { "null",   3, "writer: a bare null is a document" },
        };
        for (size_t wi = 0; wi < sizeof(wr) / sizeof(wr[0]); wi++) {
            AxlString    *wo = axl_string_new(NULL);
            AxlJsonWriter ww;
            axl_json_writer_init(&ww, wo, AXL_JSON_STRICT);
            switch (wr[wi].kind) {
            case 0:  axl_json_int(&ww, 42);    break;
            case 1:  axl_json_str(&ww, "hi");  break;
            case 2:  axl_json_bool(&ww, true); break;
            default: axl_json_null(&ww);       break;
            }
            axl_json_writer_finish(&ww);
            test_check(!axl_json_writer_error(&ww) &&
                       axl_strcmp(axl_string_str(wo), wr[wi].want) == 0,
                       wr[wi].what);
            axl_string_free(wo);
        }

        /* A second value at depth 0 is still an error -- "one value" is the
           relaxation, not "a stream of values". begin_item already had this
           guard; the assertion pins that allowing the first did not disarm it. */
        {
            AxlString    *wo = axl_string_new(NULL);
            AxlJsonWriter ww;
            axl_json_writer_init(&ww, wo, AXL_JSON_STRICT);
            axl_json_int(&ww, 1);
            axl_json_int(&ww, 2);
            axl_json_writer_finish(&ww);
            test_check(axl_json_writer_error(&ww) &&
                       axl_strcmp(axl_string_str(wo), "1") == 0,
                       "writer: a second root value sets the error, emits nothing more");
            axl_string_free(wo);
        }

        /* A key at depth 0 is still an error: relaxing the ATOM rule must not
           relax the structural one. */
        {
            AxlString    *wo = axl_string_new(NULL);
            AxlJsonWriter ww;
            axl_json_writer_init(&ww, wo, AXL_JSON_STRICT);
            axl_json_key(&ww, "k");
            axl_json_writer_finish(&ww);
            test_check(axl_json_writer_error(&ww),
                       "writer: a key at depth 0 is still a misuse");
            axl_string_free(wo);
        }

        /* Round-trip through the bridge: a bare-root document parses, and
           axl_json_write_token re-emits it byte-identically. This is the
           reader/writer agreement stated as one assertion. */
        {
            const char *docs[] = { "42", "\"hi\"", "true", "null", "-0.5e3" };
            size_t round_ok = 0;
            for (size_t ri = 0; ri < sizeof(docs) / sizeof(docs[0]); ri++) {
                AxlJsonReader rr;
                if (!axl_json_parse(docs[ri], axl_strlen(docs[ri]),
                                          AXL_JSON_STRICT, &rr)) {
                    continue;
                }
                AxlString    *wo = axl_string_new(NULL);
                AxlJsonWriter ww;
                axl_json_writer_init(&ww, wo, AXL_JSON_STRICT);
                axl_json_write_token(&ww, &rr, 0);
                axl_json_writer_finish(&ww);
                if (!axl_json_writer_error(&ww) &&
                    axl_strcmp(axl_string_str(wo), docs[ri]) == 0) {
                    round_ok++;
                }
                axl_string_free(wo);
                axl_json_free(&rr);
            }
            test_check(round_ok == sizeof(docs) / sizeof(docs[0]),
                       "writer: every bare-root document round-trips byte-identically");
        }
    }

    /* The parser's HONORING of the depth field -- the boundary either side of
       DEPTH_DEFAULT, a raised and a lowered bound, and the two recursion bombs
       JSONTestSuite ships -- lives in test/unit/axl-test-json-conformance.c.
       Deliberately a separate binary: the failure mode there is a stack
       overflow, which in UEFI is a #GP or a hang rather than a failed
       assertion, and would starve every later binary in the shared QEMU boot. */

    /* --- P3: the no-flags entry points are LIBERAL ------------------------
       axl_json_parse() and axl_json_load_file() take no flags word, so `0`
       would have meant STRICT -- and a firmware SDK reading a sidecar or an
       API response it does not control is better served by "read whatever you
       were handed". They use AXL_JSON_RELAXED. This is what makes P3 a
       widening change rather than a restricting one: every document that
       parsed before still parses, and comments now parse too. */
    {
        const char *j5 = "{ a: 0x10, /* c */ b: 'v', }";
        AxlJsonReader lr;
        int64_t la = 0;
        char lb[8] = { 0 };
        test_check(axl_json_parse(j5, axl_strlen(j5), AXL_JSON_RELAXED, &lr) &&
                   axl_json_get_int(&lr, "a", &la) && la == 0x10 &&
                   axl_json_get_string(&lr, "b", lb, sizeof(lb)) &&
                   axl_strcmp(lb, "v") == 0,
                   "liberal: axl_json_parse accepts a full JSON5 document");
        axl_json_free(&lr);

        /* And the discriminating half: the SAME document under the flags form
           with STRICT must be refused. Without this, "parse is liberal" would
           pass against a parser that had become permissive for everyone. */
        AxlJsonReader xr;
        bool strict_got = axl_json_parse(j5, axl_strlen(j5),
                                               AXL_JSON_STRICT, &xr);
        if (strict_got) { axl_json_free(&xr); }
        test_check(!strict_got,
                   "liberal: the same document under STRICT is refused");

        /* Ill-formed UTF-8 in a string does not stop the parse. Pinned for
           what it actually is -- CURRENT behavior under EVERY flag value --
           and not, as an earlier comment here claimed, as evidence that
           RELAXED's UTF8_RAW bit is doing something. It is not: the reader
           validates no encoding yet (UTF-8 modes on read are a later phase),
           so this passes identically under AXL_JSON_STRICT and could never
           have discriminated the two. Asserting both spellings is what makes
           that explicit rather than implied, and it is the RED that the
           read-side UTF8_STRICT mode will have to turn. */
        /* REGRESSION, and the reason the liberal DEFAULT needed a second look
           while there was one. `\0` is a legal JSON5 escape and
           AXL_JSON_RELAXED carries ALLOW_EXTRA_ESCAPES -- so the moment
           axl_json_parse defaulted to it, a document that named no dialect at
           all could ask an accessor to write an interior NUL into a
           NUL-TERMINATED buffer. Every axl_strcmp then sees only the prefix:
           "admin\0extra" compared EQUAL to "admin", which is a string
           smuggling primitive, and it is reachable from JWT headers/claims,
           JWKs and HTTP request bodies. The vendored jsmn refused every
           non-RFC-8259 escape, so this could not arise before.
           decode_json_string now substitutes U+FFFD, so the byte cannot
           truncate. Asserted as an EXACT string, since the whole failure mode
           is a prefix comparing equal. */
        {
            const char *nuldoc = "{\"user\":\"admin\\0extra\"}";
            AxlJsonReader nr;
            char nv[32] = { 0 };
            bool nok = axl_json_parse(nuldoc, axl_strlen(nuldoc), AXL_JSON_RELAXED, &nr) &&
                       axl_json_get_string(&nr, "user", nv, sizeof(nv));
            test_check(nok && axl_strcmp(nv, "admin") != 0,
                       "escape: a \\0 escape cannot truncate a string to a prefix");
            test_check(nok && /* Split literal: "\xBDe" would lex as ONE hex escape (0xBDE, out of
                          range) and swallow the `e` -- gcc warns, and the warning
                          was the failure. */
                       axl_strcmp(nv, "admin\xEF\xBF\xBD" "extra") == 0,
                       "escape: \\0 decodes to U+FFFD, exact");
            if (nok) { axl_json_free(&nr); }
        }

        const char *rawdoc = "{\"a\":\"\x80\"}";
        AxlJsonReader rr;
        bool raw_liberal = axl_json_parse(rawdoc, axl_strlen(rawdoc), AXL_JSON_RELAXED, &rr);
        if (raw_liberal) { axl_json_free(&rr); }
        bool raw_strict = axl_json_parse(rawdoc, axl_strlen(rawdoc),
                                               AXL_JSON_STRICT, &rr);
        if (raw_strict) { axl_json_free(&rr); }
        test_check(raw_liberal && raw_strict,
                   "liberal: ill-formed UTF-8 parses under BOTH dialects "
                   "(the reader validates no encoding yet)");
    }

    /* --- P2 parser bugs, each found by the review pass -------------------
       All four were reachable with a SINGLE dialect flag set, which is what
       made them P2's to fix: the phase's whole claim is that one flag permits
       one feature, and each of these was a feature permitted by no flag at
       all. */
    {
        struct { const char *doc; AxlJsonFlags f; bool ok; const char *what; } bug[] = {
            /* \v and \f are ES5 whitespace, not RFC 8259 whitespace. */
            { "{\"a\":\v1}", AXL_JSON_ALLOW_COMMENTS, false,
              "bug: \\v whitespace needs ALLOW_EXTRA_WHITESPACE" },
            { "{\"a\":\f1}", AXL_JSON_ALLOW_COMMENTS, false,
              "bug: \\f whitespace needs ALLOW_EXTRA_WHITESPACE" },
            { "{\"a\":\v1}", AXL_JSON_ALLOW_EXTRA_WHITESPACE, true,
              "bug: \\v whitespace accepted WITH the flag" },

            /* Leading zeros: illegal under RFC 8259 AND ES5, so ungated. */
            { "{\"a\":01}",  AXL_JSON_JSON5, false,
              "bug: leading zero rejected even under full JSON5" },
            { "{\"a\":-01}", AXL_JSON_JSON5, false,
              "bug: leading zero rejected after a sign" },
            { "{\"a\":0}",   AXL_JSON_JSON5, true,
              "bug: a bare 0 is still legal" },
            { "{\"a\":0.5}", AXL_JSON_JSON5, true,
              "bug: 0.5 is still legal (0 then a fraction)" },

            /* Raw control chars in strings: TAB is ES5-legal and gated;
               LF and CR are illegal under both specs, always. */
            { "{\"a\":\"x\ty\"}", AXL_JSON_ALLOW_COMMENTS, false,
              "bug: raw TAB in a string needs ALLOW_EXTRA_WHITESPACE" },
            { "{\"a\":\"x\ty\"}", AXL_JSON_ALLOW_EXTRA_WHITESPACE, true,
              "bug: raw TAB in a string accepted WITH the flag" },
            { "{\"a\":\"x\ny\"}", AXL_JSON_JSON5, false,
              "bug: raw LF in a string rejected even under full JSON5" },
            { "{\"a\":\"x\ry\"}", AXL_JSON_JSON5, false,
              "bug: raw CR in a string rejected even under full JSON5" },

            /* An unterminated trailing block comment used to be ACCEPTED:
               skip_ws's error was discarded, and pos landed exactly on len. */
            { "{\"a\":1}/*",   AXL_JSON_JSON5, false,
              "bug: unterminated trailing block comment is rejected" },
            { "{\"a\":1}/* */", AXL_JSON_JSON5, true,
              "bug: a CLOSED trailing block comment is still fine" },
        };
        for (size_t bi = 0; bi < sizeof(bug) / sizeof(bug[0]); bi++) {
            AxlJsonReader br;
            bool got = axl_json_parse(bug[bi].doc, axl_strlen(bug[bi].doc),
                                            bug[bi].f, &br);
            if (got) { axl_json_free(&br); }
            test_check(got == bug[bi].ok, bug[bi].what);
        }
    }

    /* Trailing commas, compact */
    out = axl_string_new(NULL);
    axl_json_writer_init(&w, out, AXL_JSON_ALLOW_TRAILING_COMMA);
    axl_json_obj_begin(&w);
    axl_json_kv_int(&w, "a", 1);
    axl_json_kv_int(&w, "b", 2);
    axl_json_obj_end(&w);
    axl_json_writer_finish(&w);
    test_check(!axl_json_writer_error(&w), "json5 build: trailing-comma no error");
    test_check(axl_strcmp(axl_string_str(out), "{\"a\":1,\"b\":2,}") == 0,
               "json5 build: trailing comma in compact object");
    /* And the JSON5 reader accepts it. */
    test_check(axl_json_parse(axl_string_str(out),
                                    axl_string_len(out),
                                    AXL_JSON_JSON5, &r),
               "json5 build: trailing-comma output round-trips through JSON5 reader");
    axl_json_free(&r);
    axl_string_free(out);

    /* Trailing commas, pretty */
    out = axl_string_new(NULL);
    axl_json_writer_init(&w, out,
        AXL_JSON_INDENT(2) | AXL_JSON_ALLOW_TRAILING_COMMA);
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
    axl_json_writer_init(&w, out, AXL_JSON_INDENT(2));
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
    test_check(axl_json_parse(axl_string_str(out),
                                    axl_string_len(out),
                                    AXL_JSON_JSON5, &r),
               "json5 build: pretty comment output round-trips");
    axl_json_free(&r);
    axl_string_free(out);

    /* Comment in compact mode emits inline / * ... * / */
    out = axl_string_new(NULL);
    axl_json_writer_init(&w, out, AXL_JSON_STRICT);
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
    axl_json_writer_init(&w, out, AXL_JSON_INDENT(2));
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
    axl_json_writer_init(&w, out, AXL_JSON_STRICT);
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
// JSON Sink Tests (P10)
// ---------------------------------------------------------------------------

/* ONE document, built by ONE function, written into every sink. The sinks are
   then checked against each OTHER rather than each against its own hand-typed
   expectation -- a per-sink expectation string can be edited to match whatever
   that sink happens to produce, which is how a formatting regression hides in
   a green suite. */
#define JSON_SINK_DOC      "{\"name\":\"devkit\",\"version\":42,\"ok\":true}"
#define JSON_SINK_DOC_LEN  (sizeof(JSON_SINK_DOC) - 1)

static void
build_sink_doc(AxlJsonWriter *w)
{
    axl_json_obj_begin(w);
    axl_json_kv_str(w, "name", "devkit");
    axl_json_kv_int(w, "version", 42);
    axl_json_kv_bool(w, "ok", true);
    axl_json_obj_end(w);
}

/* A custom AxlStream BACKEND under test control.
 *
 * This is what closes the one hole P10 shipped with. The stream sink's
 * "accepted less than asked" path was untestable when AxlStream had a closed
 * set of four backends, none of which short-transfers; axl_stream_open_custom
 * makes short transfers contractually legal AND constructible, so the path is
 * now reachable from a unit test rather than only from a real socket. */
typedef struct {
    char         buf[256];
    size_t       used;
    size_t       bite;   ///< most bytes to accept per call; 0 accepts none
    bool         fail;   ///< report -1 instead: the backend is broken
    unsigned int calls;
} StreamBackend;

static axl_ssize_t
sbe_write(void *ctx, const void *buf, size_t count)
{
    StreamBackend *b = (StreamBackend *)ctx;
    size_t         take;

    b->calls++;
    if (b->fail) {
        return -1;
    }
    take = (count < b->bite) ? count : b->bite;
    if (take > sizeof(b->buf) - b->used) {
        take = sizeof(b->buf) - b->used;
    }
    axl_memcpy(b->buf + b->used, buf, take);
    b->used += take;
    return (axl_ssize_t)take;
}

/* Wire a StreamBackend up as a stream. `close` is left NULL because the
   context is the caller's stack object, and a NULL close slot is documented
   as a no-op rather than an error. */
static AxlStream *
open_backend(StreamBackend *b)
{
    AxlStreamOps ops = AXL_STREAM_OPS_INIT;

    ops.write = sbe_write;
    return axl_stream_open_custom(b, &ops, "json-test-sink");
}

/* A callback sink under test control: it records what it was handed and how
   many times, and each variant returns a different one of the three outcomes
   an AxlJsonWriteFn can report. */
typedef struct {
    char         buf[256];
    size_t       used;
    unsigned int calls;
} SinkProbe;

static void
probe_store(SinkProbe *p, const char *buf, size_t n)
{
    for (size_t i = 0; i < n && p->used < sizeof(p->buf); i++) {
        p->buf[p->used++] = buf[i];
    }
}

/* Takes everything: the ordinary case. */
static axl_ssize_t
probe_write_all(void *ctx, const char *buf, size_t len)
{
    SinkProbe *p = (SinkProbe *)ctx;

    p->calls++;
    probe_store(p, buf, len);
    return (axl_ssize_t)len;
}

/* Broken: must HALT the writer at the first refusal. */
static axl_ssize_t
probe_write_fail(void *ctx, const char *buf, size_t len)
{
    SinkProbe *p = (SinkProbe *)ctx;

    (void)buf;
    (void)len;
    p->calls++;
    return -1;
}

/* Full, not broken: takes half of every fragment. The writer must keep going
   and keep counting, and report only at finish. */
static axl_ssize_t
probe_write_short(void *ctx, const char *buf, size_t len)
{
    SinkProbe   *p    = (SinkProbe *)ctx;
    const size_t take = len / 2;

    p->calls++;
    probe_store(p, buf, take);
    return (axl_ssize_t)take;
}

/* Claims MORE than it was given. A sink bug, and one the writer must refuse
   rather than clamp: clamping would let `written` reach `needed` and finish()
   would then certify a document that may have been truncated. */
static axl_ssize_t
probe_write_over(void *ctx, const char *buf, size_t len)
{
    SinkProbe *p = (SinkProbe *)ctx;

    (void)buf;
    p->calls++;
    return (axl_ssize_t)len + 1;
}

static void
test_json_sinks(void)
{
    AxlJsonWriter w;

    // --- the string sink, reached the long way ----------------------------
    // writer_init_sink + sink_init_string IS writer_init. If the two entry
    // points can disagree, every other sink test is measuring the wrong thing.
    {
        AXL_AUTOPTR(AxlString) out = axl_string_new(NULL);
        AxlJsonSink            snk;

        axl_json_sink_init_string(&snk, out);
        axl_json_writer_init_sink(&w, &snk, AXL_JSON_STRICT);
        build_sink_doc(&w);
        const size_t n = axl_json_writer_finish(&w);

        test_check(!axl_json_writer_error(&w)
                   && axl_strcmp(axl_string_str(out), JSON_SINK_DOC) == 0,
                   "sink string: writer_init_sink builds the same document as "
                   "writer_init");
        test_check(n == JSON_SINK_DOC_LEN
                   && axl_json_writer_written(&w) == JSON_SINK_DOC_LEN
                   && axl_json_writer_needed(&w) == JSON_SINK_DOC_LEN,
                   "sink string: finish, written and needed all agree when "
                   "nothing is dropped");
    }

    // --- finish() counts THIS writer's bytes, not the string's length -----
    // It used to return axl_string_len(out), and every call site in the tree
    // happens to pass a fresh AxlString, so the two agreed everywhere and the
    // change was invisible. Pinned with a PRE-POPULATED string, which is the
    // only shape that can tell them apart.
    {
        AXL_AUTOPTR(AxlString) out = axl_string_new("PREFIX");
        AxlJsonSink            snk;

        axl_json_sink_init_string(&snk, out);
        axl_json_writer_init_sink(&w, &snk, AXL_JSON_STRICT);
        axl_json_obj_begin(&w);
        axl_json_obj_end(&w);
        const size_t n = axl_json_writer_finish(&w);

        test_check(n == 2 && axl_json_writer_written(&w) == 2
                   && axl_json_writer_needed(&w) == 2,
                   "sink string: finish counts what THIS writer emitted, not "
                   "what was already in the string");
        test_check(axl_strcmp(axl_string_str(out), "PREFIX{}") == 0,
                   "sink string: the writer APPENDS — it does not clear the "
                   "caller's string");
    }

    // --- the sink is COPIED, and need not outlive init --------------------
    // The header promises this explicitly, and the previous draft of the
    // contract could not honor it: its buffer sink kept state inside the sink
    // and had to pass a self-pointer as its own context.
    {
        AXL_AUTOPTR(AxlString) out = axl_string_new(NULL);

        {
            AxlJsonSink tmp;

            axl_json_sink_init_string(&tmp, out);
            axl_json_writer_init_sink(&w, &tmp, AXL_JSON_STRICT);
            /* Scribble it the way a dead stack frame would. */
            axl_memset(&tmp, 0xAA, sizeof(tmp));
        }
        build_sink_doc(&w);
        axl_json_writer_finish(&w);

        test_check(!axl_json_writer_error(&w)
                   && axl_strcmp(axl_string_str(out), JSON_SINK_DOC) == 0,
                   "sink copy: overwriting the caller's sink after init "
                   "changes nothing");
    }

    // --- buffer sink at size 0: the load-bearing case ---------------------
    // If a full buffer latched the sticky error, the writer would halt at the
    // FIRST fragment and `needed` could only ever report that fragment. Two-
    // pass sizing would then be impossible and this test says so by execution.
    {
        AxlJsonSink    snk;
        AxlJsonBufSink st;

        axl_json_sink_init_buffer(&snk, &st, NULL, 0);
        axl_json_writer_init_sink(&w, &snk, AXL_JSON_STRICT);
        build_sink_doc(&w);
        const size_t n = axl_json_writer_finish(&w);

        test_check(axl_json_writer_needed(&w) == JSON_SINK_DOC_LEN,
                   "sink buffer: a zero-sized sink still counts the WHOLE "
                   "document");
        test_check(axl_json_writer_written(&w) == 0 && n == 0 && st.used == 0,
                   "sink buffer: nothing is stored at size 0");
        test_check(axl_json_writer_error(&w)
                   && axl_json_writer_error_info(&w)->code == AXL_JSON_ERR_IO,
                   "sink buffer: finish reports IO once for a document that "
                   "did not fit");
    }

    // --- two-pass sizing, end to end --------------------------------------
    {
        AxlJsonSink    snk;
        AxlJsonBufSink st;

        axl_json_sink_init_buffer(&snk, &st, NULL, 0);
        axl_json_writer_init_sink(&w, &snk, AXL_JSON_STRICT);
        build_sink_doc(&w);
        axl_json_writer_finish(&w);

        const size_t need = axl_json_writer_needed(&w);
        char        *buf  = axl_malloc(need);

        axl_json_sink_init_buffer(&snk, &st, buf, need);
        axl_json_writer_init_sink(&w, &snk, AXL_JSON_STRICT);
        build_sink_doc(&w);
        const size_t n = axl_json_writer_finish(&w);

        test_check(buf != NULL && !axl_json_writer_error(&w)
                   && n == need && st.used == need
                   && axl_memcmp(buf, JSON_SINK_DOC, need) == 0,
                   "sink buffer: sizing pass then an exact allocation writes "
                   "the whole document with no error");
        axl_free(buf);
    }

    // --- EVERY capacity, not a hand-picked one ----------------------------
    // The invariant comes from what a byte sink IS, not from how this one is
    // written: at capacity k the buffer holds exactly the document's first
    // min(k, needed) bytes, nothing lands past k, and the error fires exactly
    // when something was dropped.
    //
    // Claimed for every k because ONE k proves too little, and sabotage is how
    // that was found: a sink that drops a straddling fragment WHOLE instead of
    // storing the part that fits passed a hand-written "one byte short" test,
    // because this document's last fragment is one byte and k = len-1 lands
    // exactly on a fragment boundary. Only a k that CUTS a fragment can tell
    // the two apart, and sweeping is how you stop having to guess which k does.
    {
        size_t bad_cap = (size_t)-1;

        for (size_t cap = 0; cap <= JSON_SINK_DOC_LEN + 2; cap++) {
            AxlJsonSink    snk;
            AxlJsonBufSink st;
            char           buf[JSON_SINK_DOC_LEN + 4];
            const size_t   want = (cap < JSON_SINK_DOC_LEN)
                                      ? cap : JSON_SINK_DOC_LEN;

            axl_memset(buf, '#', sizeof(buf));
            axl_json_sink_init_buffer(&snk, &st, buf, cap);
            axl_json_writer_init_sink(&w, &snk, AXL_JSON_STRICT);
            build_sink_doc(&w);
            axl_json_writer_finish(&w);

            if (!(axl_json_writer_needed(&w) == JSON_SINK_DOC_LEN
                  && axl_json_writer_written(&w) == want
                  && st.used == want
                  && axl_memcmp(buf, JSON_SINK_DOC, want) == 0
                  && buf[want] == '#'
                  && axl_json_writer_error(&w) == (cap < JSON_SINK_DOC_LEN))) {
                bad_cap = cap;
                break;
            }
        }
        test_check(bad_cap == (size_t)-1,
                   "sink buffer: at every capacity the buffer holds exactly "
                   "the document's leading min(size, needed) bytes, nothing "
                   "past it, and errors exactly when it truncated");
    }

    // --- callback sink, taking everything ---------------------------------
    {
        SinkProbe   p = { .used = 0, .calls = 0 };
        AxlJsonSink snk;

        axl_json_sink_init_callback(&snk, probe_write_all, &p);
        axl_json_writer_init_sink(&w, &snk, AXL_JSON_STRICT);
        build_sink_doc(&w);
        axl_json_writer_finish(&w);

        test_check(!axl_json_writer_error(&w)
                   && p.used == JSON_SINK_DOC_LEN
                   && axl_memcmp(p.buf, JSON_SINK_DOC, JSON_SINK_DOC_LEN) == 0,
                   "sink callback: the concatenated fragments are the "
                   "document");
        test_check(p.calls > 1,
                   "sink callback: the writer streams — a document arrives as "
                   "several fragments, not one");
    }

    // --- callback sink that is BROKEN: halts at once ----------------------
    {
        SinkProbe   p = { .used = 0, .calls = 0 };
        AxlJsonSink snk;

        axl_json_sink_init_callback(&snk, probe_write_fail, &p);
        axl_json_writer_init_sink(&w, &snk, AXL_JSON_STRICT);
        build_sink_doc(&w);
        axl_json_writer_finish(&w);

        test_check(axl_json_writer_error_info(&w)->code == AXL_JSON_ERR_IO,
                   "sink callback: -1 latches IO");
        test_check(p.calls == 1,
                   "sink callback: -1 HALTS — the writer does not keep pushing "
                   "at a sink that said it is broken");
        test_check(axl_json_writer_needed(&w) == 0
                   && axl_json_writer_error_info(&w)->offset == 0,
                   "sink callback: the failing fragment is not counted, so the "
                   "error offset is where the writer got TO");
    }

    // --- callback sink that is merely FULL: keeps going --------------------
    // The distinction the whole design rests on. A short count must not halt.
    {
        SinkProbe   p = { .used = 0, .calls = 0 };
        AxlJsonSink snk;

        axl_json_sink_init_callback(&snk, probe_write_short, &p);
        axl_json_writer_init_sink(&w, &snk, AXL_JSON_STRICT);
        build_sink_doc(&w);
        axl_json_writer_finish(&w);

        test_check(p.calls > 1,
                   "sink callback: a short count does NOT halt the writer");
        test_check(axl_json_writer_needed(&w) == JSON_SINK_DOC_LEN,
                   "sink callback: a short count still counts the whole "
                   "document");
        test_check(axl_json_writer_written(&w) < JSON_SINK_DOC_LEN
                   && axl_json_writer_written(&w) == p.used,
                   "sink callback: written is the sum of what the sink "
                   "actually took");
        test_check(axl_json_writer_error_info(&w)->code == AXL_JSON_ERR_IO,
                   "sink callback: the drop is reported once, at finish");
    }

    // --- callback sink that over-reports -----------------------------------
    {
        SinkProbe   p = { .used = 0, .calls = 0 };
        AxlJsonSink snk;

        axl_json_sink_init_callback(&snk, probe_write_over, &p);
        axl_json_writer_init_sink(&w, &snk, AXL_JSON_STRICT);
        build_sink_doc(&w);
        axl_json_writer_finish(&w);

        test_check(axl_json_writer_error_info(&w)->code == AXL_JSON_ERR_IO
                   && p.calls == 1,
                   "sink callback: a count larger than the fragment is refused "
                   "as a broken sink, not clamped");
    }

    // --- stream sink -------------------------------------------------------
    {
        AxlStream  *s = axl_bufopen();
        AxlJsonSink snk;
        size_t      size = 0;

        axl_json_sink_init_stream(&snk, s);
        axl_json_writer_init_sink(&w, &snk, AXL_JSON_STRICT);
        build_sink_doc(&w);
        axl_json_writer_finish(&w);

        const char *data = (const char *)axl_bufdata(s, &size);

        test_check(!axl_json_writer_error(&w) && size == JSON_SINK_DOC_LEN
                   && data != NULL
                   && axl_memcmp(data, JSON_SINK_DOC, JSON_SINK_DOC_LEN) == 0,
                   "sink stream: the stream holds exactly the document");
        axl_fclose(s);
    }

    // --- a stream backend that accepts ONE byte per call ------------------
    // The whole document must still land: axl_fwrite loops until the request
    // is satisfied, which is precisely what "short transfers are handled
    // above the backend" means. A sink that issued one raw axl_write and
    // treated a short count as a malfunction would truncate here.
    //
    // One byte, not four. Four passed against the UNFIXED sink, because the
    // writer's longest fragment is the 4-byte literal `true` and nothing
    // straddled -- the same hand-picked-boundary trap the buffer-capacity
    // sweep was written to escape. A bite of 1 is short for every fragment
    // the writer can emit except a single delimiter.
    {
        StreamBackend b = { .used = 0, .bite = 1, .fail = false, .calls = 0 };
        AxlStream    *s = open_backend(&b);
        AxlJsonSink   snk;

        axl_json_sink_init_stream(&snk, s);
        axl_json_writer_init_sink(&w, &snk, AXL_JSON_STRICT);
        build_sink_doc(&w);
        axl_json_writer_finish(&w);

        test_check(!axl_json_writer_error(&w)
                   && b.used == JSON_SINK_DOC_LEN
                   && axl_memcmp(b.buf, JSON_SINK_DOC, JSON_SINK_DOC_LEN) == 0,
                   "sink stream: a backend taking one byte at a time still "
                   "receives the whole document");
        test_check(axl_json_writer_written(&w) == JSON_SINK_DOC_LEN
                   && axl_json_writer_needed(&w) == JSON_SINK_DOC_LEN,
                   "sink stream: a short BACKEND transfer is not a short "
                   "write — the counters see a complete document");
        axl_fclose(s);
    }

    // --- a backend that accepts NOTHING, without being broken -------------
    // axl_ferror() is what separates the two, and it is the reason this sink
    // can no longer map every short write to -1: "would not take more right
    // now" is a documented, non-error outcome of a custom backend.
    {
        StreamBackend b = { .used = 0, .bite = 0, .fail = false, .calls = 0 };
        AxlStream    *s = open_backend(&b);
        AxlJsonSink   snk;

        axl_json_sink_init_stream(&snk, s);
        axl_json_writer_init_sink(&w, &snk, AXL_JSON_STRICT);
        build_sink_doc(&w);
        axl_json_writer_finish(&w);

        test_check(axl_json_writer_needed(&w) == JSON_SINK_DOC_LEN
                   && axl_json_writer_written(&w) == 0,
                   "sink stream: a backend that takes nothing is FULL, not "
                   "broken — the writer keeps counting the document");
        test_check(b.calls > 1,
                   "sink stream: and the writer is not halted by it");
        test_check(axl_json_writer_error_info(&w)->code == AXL_JSON_ERR_IO,
                   "sink stream: the drop is reported once, at finish");
        axl_fclose(s);
    }

    // --- a backend that is genuinely BROKEN -------------------------------
    {
        StreamBackend b = { .used = 0, .bite = 4, .fail = true, .calls = 0 };
        AxlStream    *s = open_backend(&b);
        AxlJsonSink   snk;

        axl_json_sink_init_stream(&snk, s);
        axl_json_writer_init_sink(&w, &snk, AXL_JSON_STRICT);
        build_sink_doc(&w);
        axl_json_writer_finish(&w);

        test_check(axl_json_writer_error_info(&w)->code == AXL_JSON_ERR_IO
                   && axl_json_writer_needed(&w) == 0 && b.calls == 1,
                   "sink stream: a backend reporting -1 HALTS the writer at "
                   "the first fragment");
        axl_fclose(s);
    }

    // --- a stream that cannot be written at all ---------------------------
    // axl_stdin has no write slot. That is a CALLER bug, not backpressure,
    // and the write path cannot tell the difference for itself: axl_write
    // answers -1 at its own NULL-slot guard without setting the stream's
    // error flag, so axl_ferror() stays false and it looks exactly like a
    // backend that took nothing. Hence the check belongs at init, where the
    // honest code is INVALID_ARGUMENT.
    {
        AxlJsonSink snk;

        axl_json_sink_init_stream(&snk, axl_stdin);
        axl_memset(&w, 0xAA, sizeof(w));
        axl_json_writer_init_sink(&w, &snk, AXL_JSON_STRICT);
        const size_t n = axl_json_writer_finish(&w);

        test_check(axl_json_writer_error_info(&w)->code
                   == AXL_JSON_ERR_INVALID_ARGUMENT,
                   "sink stream: a stream with no write capability is refused "
                   "at init, not misread as backpressure");
        test_check(n == 0 && axl_json_writer_written(&w) == 0
                   && axl_json_writer_needed(&w) == 0,
                   "sink stream: and nothing is counted or emitted through it");
    }

    // --- argument paths -----------------------------------------------------
    {
        AxlJsonSink    snk;
        AxlJsonBufSink st;
        char           buf[8];

        axl_memset(&w, 0xAA, sizeof(w));
        axl_json_writer_init_sink(&w, NULL, AXL_JSON_STRICT);
        test_check(axl_json_writer_error(&w)
                   && axl_json_writer_error_info(&w)->code
                      == AXL_JSON_ERR_INVALID_ARGUMENT,
                   "sink args: a NULL sink reports INVALID_ARGUMENT, not IO");

        snk.write = NULL;
        snk.ctx   = NULL;
        axl_memset(&w, 0xAA, sizeof(w));
        axl_json_writer_init_sink(&w, &snk, AXL_JSON_STRICT);
        test_check(axl_json_writer_error(&w)
                   && axl_json_writer_error_info(&w)->code
                      == AXL_JSON_ERR_INVALID_ARGUMENT,
                   "sink args: a sink with no write function reports "
                   "INVALID_ARGUMENT");

        /* A buffer sink with nowhere to keep its own state is the same caller
           bug, and must be refused at init rather than dereferenced later. */
        axl_json_sink_init_buffer(&snk, NULL, buf, sizeof(buf));
        axl_memset(&w, 0xAA, sizeof(w));
        axl_json_writer_init_sink(&w, &snk, AXL_JSON_STRICT);
        test_check(axl_json_writer_error_info(&w)->code
                   == AXL_JSON_ERR_INVALID_ARGUMENT,
                   "sink args: a buffer sink with no state object is refused "
                   "at init");

        /* A capacity with no buffer under it. The zero-size sizing pass says
           NULL is legitimate; NULL with room to write is a caller bug. */
        axl_json_sink_init_buffer(&snk, &st, NULL, 16);
        axl_memset(&w, 0xAA, sizeof(w));
        axl_json_writer_init_sink(&w, &snk, AXL_JSON_STRICT);
        test_check(axl_json_writer_error_info(&w)->code
                   == AXL_JSON_ERR_INVALID_ARGUMENT,
                   "sink args: a NULL buffer with a non-zero size is refused, "
                   "while NULL at size 0 is the sizing pass");

        axl_json_sink_init_stream(&snk, NULL);
        axl_memset(&w, 0xAA, sizeof(w));
        axl_json_writer_init_sink(&w, &snk, AXL_JSON_STRICT);
        test_check(axl_json_writer_error_info(&w)->code
                   == AXL_JSON_ERR_INVALID_ARGUMENT,
                   "sink args: a stream sink with no stream is refused at "
                   "init");

        test_check(axl_json_writer_written(NULL) == 0
                   && axl_json_writer_needed(NULL) == 0,
                   "sink args: the counters are NULL-safe");

        axl_json_sink_init_string(NULL, NULL);
        axl_json_sink_init_buffer(NULL, &st, buf, sizeof(buf));
        axl_json_sink_init_stream(NULL, NULL);
        axl_json_sink_init_callback(NULL, probe_write_all, NULL);
        test_survived("sink args: a NULL sink initializer is a no-op");
    }
}

// ---------------------------------------------------------------------------
// JSON Source Tests (P10)
// ---------------------------------------------------------------------------

#define JSON_SRC_DOC "{\"k\":\"AAAA\",\"n\":7}"
/* Offset of the first byte INSIDE the "AAAA" value. The zero-copy tests
   mutate it behind the reader's back: a borrowing reader sees the new byte,
   a reader that copied does not. */
#define JSON_SRC_PAYLOAD 6

/* A pull source under test control. It hands the document over in small
   BITES so the reader's accumulate-and-grow loop is genuinely exercised
   rather than swallowing the document in one read, and each flag turns on
   one of the ways a read function can misbehave. */
typedef struct {
    const char  *doc;
    size_t       len;
    size_t       pos;
    size_t       bite;   ///< most bytes to hand over per call
    unsigned int calls;
    bool         fail;   ///< report -1 once something has been read
    bool         over;   ///< claim MORE bytes than the buffer can hold
} SrcProbe;

static axl_ssize_t
probe_read(void *ctx, void *buf, size_t max)
{
    SrcProbe *p = (SrcProbe *)ctx;
    size_t    n;

    p->calls++;
    if (p->fail && p->calls >= 2) {
        return -1;
    }
    if (p->over) {
        return (axl_ssize_t)max + 1;   /* writes nothing; claims everything */
    }
    n = p->len - p->pos;
    if (n > max) {
        n = max;
    }
    if (n > p->bite) {
        n = p->bite;
    }
    axl_memcpy(buf, p->doc + p->pos, n);
    p->pos += n;
    return (axl_ssize_t)n;
}

static void
src_probe_init(SrcProbe *p, const char *doc, size_t len, size_t bite)
{
    p->doc   = doc;
    p->len   = len;
    p->pos   = 0;
    p->bite  = bite;
    p->calls = 0;
    p->fail  = false;
    p->over  = false;
}

static void
test_json_sources(void)
{
    AxlJsonSource src;
    AxlJsonReader r;
    char          val[16];
    int64_t       num;

    // --- the contiguous source: the fast path, unchanged ------------------
    {
        axl_json_source_init_mem(&src, JSON_SRC_DOC, sizeof(JSON_SRC_DOC) - 1);
        num = 0;
        test_check(axl_json_parse_source(&src, AXL_JSON_STRICT, &r)
                   && axl_json_get_string(&r, "k", val, sizeof(val))
                   && axl_strcmp(val, "AAAA") == 0
                   && axl_json_get_int(&r, "n", &num) && num == 7,
                   "source mem: parses exactly what axl_json_parse "
                   "parses");
        axl_json_free(&r);
    }

    // --- and it BORROWS ---------------------------------------------------
    // Zero-copy stated as behaviour rather than as a claim about the
    // implementation: mutate the caller's buffer after the parse and the
    // reader must see the new byte. A reader that copied cannot pass this,
    // and no field has to be peeked at to find out.
    {
        char doc[] = JSON_SRC_DOC;

        axl_json_source_init_mem(&src, doc, sizeof(doc) - 1);
        axl_json_parse_source(&src, AXL_JSON_STRICT, &r);
        doc[JSON_SRC_PAYLOAD] = 'B';
        test_check(axl_json_get_string(&r, "k", val, sizeof(val))
                   && axl_strcmp(val, "BAAA") == 0,
                   "source mem: BORROWS — a mutation to the caller's buffer "
                   "shows through the reader, so nothing was copied");
        axl_json_free(&r);
    }

    // --- the stream source COPIES, and the reader owns the copy -----------
    {
        char        doc[] = JSON_SRC_DOC;
        AxlMemStats before, after;
        AxlStream  *s;

        /* The snapshot has to bracket the STREAM too, not just the parse: it
           is taken before axl_bufopen() and read after axl_fclose(), so the
           stream's own allocations cancel instead of showing up as a negative
           and making the comparison meaningless. */
        axl_mem_get_stats(&before);
        s = axl_bufopen();
        axl_fwrite(doc, 1, sizeof(doc) - 1, s);
        axl_fseek(s, 0, AXL_SEEK_SET);

        axl_json_source_init_stream(&src, s);
        const bool ok = axl_json_parse_source(&src, AXL_JSON_STRICT, &r);

        /* Scribble the ORIGINAL. A reader that borrowed the stream's buffer
           would be unaffected by this, so the check that bites is the one
           below it -- this only proves the caller's array is not the
           reader's. */
        axl_memset(doc, '?', sizeof(doc));
        test_check(ok && axl_json_get_string(&r, "k", val, sizeof(val))
                   && axl_strcmp(val, "AAAA") == 0,
                   "source stream: reads the whole document out of a stream");

        axl_json_free(&r);
        axl_fclose(s);
        axl_mem_get_stats(&after);
        test_check(after.count == before.count && after.bytes == before.bytes,
                   "source stream: axl_json_free releases the bytes the reader "
                   "accumulated — a streamed parse is ONE object to free");
    }

    // --- the callback source, handed the document one byte at a time ------
    {
        SrcProbe p;

        src_probe_init(&p, JSON_SRC_DOC, sizeof(JSON_SRC_DOC) - 1, 1);
        axl_json_source_init_callback(&src, probe_read, &p, 0);
        num = 0;
        test_check(axl_json_parse_source(&src, AXL_JSON_STRICT, &r)
                   && axl_json_get_string(&r, "k", val, sizeof(val))
                   && axl_strcmp(val, "AAAA") == 0
                   && axl_json_get_int(&r, "n", &num) && num == 7,
                   "source callback: one byte per read still assembles the "
                   "whole document");
        test_check(p.calls > sizeof(JSON_SRC_DOC) - 1,
                   "source callback: the reader kept pulling until EOF rather "
                   "than trusting the first read");
        axl_json_free(&r);
    }

    // --- a size hint must change nothing but the allocation pattern -------
    {
        SrcProbe p;

        src_probe_init(&p, JSON_SRC_DOC, sizeof(JSON_SRC_DOC) - 1, 4);
        axl_json_source_init_callback(&src, probe_read, &p,
                                      sizeof(JSON_SRC_DOC) - 1);
        test_check(axl_json_parse_source(&src, AXL_JSON_STRICT, &r)
                   && axl_json_get_string(&r, "k", val, sizeof(val))
                   && axl_strcmp(val, "AAAA") == 0,
                   "source callback: an exact size hint parses the same "
                   "document");
        axl_json_free(&r);

        /* A hint that LIES short must not truncate the document -- it is a
           hint about allocation, not a promise about length. */
        src_probe_init(&p, JSON_SRC_DOC, sizeof(JSON_SRC_DOC) - 1, 4);
        axl_json_source_init_callback(&src, probe_read, &p, 2);
        test_check(axl_json_parse_source(&src, AXL_JSON_STRICT, &r)
                   && axl_json_get_string(&r, "k", val, sizeof(val))
                   && axl_strcmp(val, "AAAA") == 0,
                   "source callback: a hint smaller than the document does not "
                   "truncate it");
        axl_json_free(&r);
    }

    // --- a read that FAILS is IO, not INCOMPLETE --------------------------
    // The distinction the signed return type exists for: INCOMPLETE tells a
    // caller to come back with more bytes, which is right after a short read
    // and wrong after a dead socket.
    {
        SrcProbe    p;
        AxlMemStats before, after;

        axl_mem_get_stats(&before);
        src_probe_init(&p, JSON_SRC_DOC, sizeof(JSON_SRC_DOC) - 1, 4);
        p.fail = true;
        axl_json_source_init_callback(&src, probe_read, &p, 0);
        test_check(!axl_json_parse_source(&src, AXL_JSON_STRICT, &r)
                   && axl_json_reader_error(&r)->code == AXL_JSON_ERR_IO,
                   "source callback: a read error reports IO, not INCOMPLETE");
        axl_json_free(&r);
        axl_mem_get_stats(&after);
        test_check(after.count == before.count && after.bytes == before.bytes,
                   "source callback: a parse abandoned mid-READ leaves nothing "
                   "allocated behind it");
    }

    // --- input that simply runs out is INCOMPLETE -------------------------
    {
        static const char kCut[] = "{\"k\":\"AAA";
        SrcProbe          p;
        AxlMemStats       before, after;

        axl_mem_get_stats(&before);
        src_probe_init(&p, kCut, sizeof(kCut) - 1, 4);
        axl_json_source_init_callback(&src, probe_read, &p, 0);
        test_check(!axl_json_parse_source(&src, AXL_JSON_STRICT, &r)
                   && axl_json_reader_error(&r)->code
                      == AXL_JSON_ERR_INCOMPLETE,
                   "source callback: input that ends mid-value reports "
                   "INCOMPLETE");
        axl_json_free(&r);
        axl_mem_get_stats(&after);
        /* A DIFFERENT cleanup path from the mid-read failure above, and
           sabotage is what showed they are different: this one read its bytes
           successfully and then failed in the PARSER, so the accumulated
           buffer is released by axl_json_parse_source rather than by the read
           loop. Leaking it left every test green. */
        test_check(after.count == before.count && after.bytes == before.bytes,
                   "source callback: a document that ARRIVED and then failed "
                   "to parse releases its bytes too");

        /* An empty stream is the same story at offset 0: a document that has
           not arrived yet, NOT a caller who passed a bad argument. */
        src_probe_init(&p, kCut, 0, 4);
        axl_json_source_init_callback(&src, probe_read, &p, 0);
        test_check(!axl_json_parse_source(&src, AXL_JSON_STRICT, &r)
                   && axl_json_reader_error(&r)->code
                      == AXL_JSON_ERR_INCOMPLETE,
                   "source callback: a source that yields no bytes at all is "
                   "INCOMPLETE, not INVALID_ARGUMENT");
        axl_json_free(&r);
    }

    // --- a read function that over-reports --------------------------------
    // It claims more bytes than the buffer it was handed. Believing it would
    // advance the accumulator past the allocation -- a heap overflow driven
    // by a caller's own callback, so it is refused rather than trusted.
    {
        SrcProbe p;

        src_probe_init(&p, JSON_SRC_DOC, sizeof(JSON_SRC_DOC) - 1, 4);
        p.over = true;
        axl_json_source_init_callback(&src, probe_read, &p, 0);
        test_check(!axl_json_parse_source(&src, AXL_JSON_STRICT, &r)
                   && axl_json_reader_error(&r)->code == AXL_JSON_ERR_IO,
                   "source callback: a read claiming more than the buffer "
                   "holds is refused, not believed");
        axl_json_free(&r);
    }

    // --- a stream that cannot be READ -------------------------------------
    // axl_stdout has no read function, so axl_read answers -1 immediately.
    // Without this, a source whose stream DIES mid-document could have been
    // mapped to EOF and the truncated document would have parsed clean — the
    // accept-and-corrupt failure this library keeps refusing.
    {
        axl_json_source_init_stream(&src, axl_stdout);
        test_check(!axl_json_parse_source(&src, AXL_JSON_STRICT, &r)
                   && axl_json_reader_error(&r)->code == AXL_JSON_ERR_IO,
                   "source stream: a stream that cannot be read reports IO, "
                   "not an empty document");
        axl_json_free(&r);
    }

    // --- argument paths ----------------------------------------------------
    {
        AxlJsonSource empty = { NULL, 0, NULL, NULL, 0 };

        /* The mirror of "a stream sink with no stream is refused at init".
           Left to the read this would surface as IO — the code for the world
           being broken — when it is a caller bug. */
        axl_json_source_init_stream(&src, NULL);
        test_check(!axl_json_parse_source(&src, AXL_JSON_STRICT, &r)
                   && axl_json_reader_error(&r)->code
                      == AXL_JSON_ERR_INVALID_ARGUMENT,
                   "source args: a stream source with no stream is refused as "
                   "INVALID_ARGUMENT, matching the sink");

        /* Empty input is answered differently by the two modes ON PURPOSE,
           and both sides are pinned so the asymmetry cannot drift into being
           accidental: a pull source that yields nothing is INCOMPLETE (above)
           because the bytes may still be coming, while a caller who says
           "here is the document" and hands over zero bytes made a mistake. */
        axl_json_source_init_mem(&src, JSON_SRC_DOC, 0);
        test_check(!axl_json_parse_source(&src, AXL_JSON_STRICT, &r)
                   && axl_json_reader_error(&r)->code
                      == AXL_JSON_ERR_INVALID_ARGUMENT,
                   "source args: a contiguous source of length 0 is "
                   "INVALID_ARGUMENT, where an empty PULL source is "
                   "INCOMPLETE");

        test_check(!axl_json_parse_source(&empty, AXL_JSON_STRICT, &r)
                   && axl_json_reader_error(&r)->code
                      == AXL_JSON_ERR_INVALID_ARGUMENT,
                   "source args: a source with neither a view nor a read "
                   "function is refused, not dereferenced");

        test_check(!axl_json_parse_source(NULL, AXL_JSON_STRICT, &r)
                   && axl_json_reader_error(&r)->code
                      == AXL_JSON_ERR_INVALID_ARGUMENT,
                   "source args: a NULL source reports INVALID_ARGUMENT");

        axl_json_source_init_mem(&src, JSON_SRC_DOC, sizeof(JSON_SRC_DOC) - 1);
        test_check(!axl_json_parse_source(&src, AXL_JSON_STRICT, NULL),
                   "source args: a NULL reader fails without writing "
                   "anywhere");

        axl_json_source_init_mem(NULL, JSON_SRC_DOC, 1);
        axl_json_source_init_stream(NULL, NULL);
        axl_json_source_init_callback(NULL, probe_read, NULL, 0);
        test_survived("source args: a NULL source initializer is a no-op");
    }
}

// ---------------------------------------------------------------------------
// The own-value mirror (P11) — the phase exists because [1,2,3] was unreadable
// ---------------------------------------------------------------------------

static void
test_json_value_mirror(void)
{
    AxlJsonReader    r, elem, sub;
    AxlJsonArrayIter it;
    char             buf[32];
    int64_t          i64;
    uint64_t         u64;
    bool             b;

    // --- THE headline: a bare-number array ---------------------------------
    // Only axl_json_value_string existed, so array_next handed back a
    // sub-reader per element and nothing could read a number out of one.
    {
        const char *doc = "[1,2,3]";
        int64_t     sum = 0;
        int         n   = 0;

        test_check(axl_json_parse(doc, axl_strlen(doc), AXL_JSON_RELAXED, &r)
                   && axl_json_value_array_begin(&r, &it),
                   "value mirror: a bare-number array opens");
        while (axl_json_array_next(&it, &elem)) {
            if (axl_json_value_int(&elem, &i64)) {
                sum += i64;
            }
            n++;
        }
        test_check(n == 3 && sum == 6,
                   "value mirror: [1,2,3] reads as 1, 2, 3 — the gap this "
                   "phase exists to close");
        axl_json_free(&r);
    }

    // --- the rest of the mirror, one element per type ----------------------
    {
        const char *doc = "[7, 18446744073709551615, true, \"s\", 1.5, null, "
                          "[9], {\"k\":1}]";

        test_check(axl_json_parse(doc, axl_strlen(doc), AXL_JSON_RELAXED, &r)
                   && axl_json_value_array_begin(&r, &it),
                   "value mirror: mixed-type array opens");

        /* 7 */
        test_check(axl_json_array_next(&it, &elem)
                   && axl_json_value_type(&elem) == AXL_JSON_TYPE_NUMBER
                   && axl_json_value_int(&elem, &i64) && i64 == 7
                   && axl_json_value_uint(&elem, &u64) && u64 == 7u,
                   "value mirror: an integer reads as NUMBER, int and uint");

        /* UINT64_MAX — the half a narrowing accessor cannot reach */
        test_check(axl_json_array_next(&it, &elem)
                   && axl_json_value_uint(&elem, &u64)
                   && u64 == 18446744073709551615ull
                   && !axl_json_value_int(&elem, &i64),
                   "value mirror: uint covers the full 64-bit range where int "
                   "must refuse");

        /* true */
        test_check(axl_json_array_next(&it, &elem)
                   && axl_json_value_type(&elem) == AXL_JSON_TYPE_BOOL
                   && axl_json_value_bool(&elem, &b) && b
                   && !axl_json_value_int(&elem, &i64),
                   "value mirror: a bool reads as BOOL, and not as a number");

        /* "s" */
        test_check(axl_json_array_next(&it, &elem)
                   && axl_json_value_type(&elem) == AXL_JSON_TYPE_STRING
                   && axl_json_value_string(&elem, buf, sizeof(buf))
                   && axl_strcmp(buf, "s") == 0
                   && !axl_json_value_number_str(&elem, buf, sizeof(buf)),
                   "value mirror: a string reads as STRING, and _number_str "
                   "refuses it");

        /* 1.5 — the documented trap: NUMBER, but value_int TRUNCATES */
        test_check(axl_json_array_next(&it, &elem)
                   && axl_json_value_type(&elem) == AXL_JSON_TYPE_NUMBER
                   && axl_json_value_int(&elem, &i64) && i64 == 1
                   && axl_json_value_number_str(&elem, buf, sizeof(buf))
                   && axl_strcmp(buf, "1.5") == 0,
                   "value mirror: 1.5 is NUMBER and value_int TRUNCATES it to "
                   "1 — the literal is how you see that");

        /* null */
        test_check(axl_json_array_next(&it, &elem)
                   && axl_json_value_type(&elem) == AXL_JSON_TYPE_NULL
                   && !axl_json_value_bool(&elem, &b),
                   "value mirror: null reads as NULL and is not a bool");

        /* [9] — nested array, walked with the same call */
        test_check(axl_json_array_next(&it, &elem)
                   && axl_json_value_type(&elem) == AXL_JSON_TYPE_ARRAY,
                   "value mirror: a nested array reads as ARRAY");
        {
            AxlJsonArrayIter inner;

            test_check(axl_json_value_array_begin(&elem, &inner)
                       && axl_json_array_next(&inner, &sub)
                       && axl_json_value_int(&sub, &i64) && i64 == 9,
                       "value mirror: and opens with the same call, no root "
                       "required");
        }

        /* {"k":1} — an element that IS an object needs no value_object */
        test_check(axl_json_array_next(&it, &elem)
                   && axl_json_value_type(&elem) == AXL_JSON_TYPE_OBJECT
                   && axl_json_get_int(&elem, "k", &i64) && i64 == 1,
                   "value mirror: an object element is already an object "
                   "context — the by-key getters apply directly");

        test_check(!axl_json_array_next(&it, &elem),
                   "value mirror: the array ends after its eight elements");
        axl_json_free(&r);
    }

    // --- a STRING that looks like a number is still a string --------------
    // The token-type guard is what enforces this, and sabotage showed nothing
    // else did: every other case here hands value_int a PRIMITIVE token, so
    // removing the guard was invisible. Drop it and `"123"` reads as 123 —
    // JSON's one real type distinction, quietly erased.
    {
        const char *doc = "[\"123\", \"1.5\", \"true\"]";

        test_check(axl_json_parse(doc, axl_strlen(doc), AXL_JSON_RELAXED, &r)
                   && axl_json_value_array_begin(&r, &it),
                   "value mirror: quoted-number array opens");

        test_check(axl_json_array_next(&it, &elem)
                   && axl_json_value_type(&elem) == AXL_JSON_TYPE_STRING
                   && !axl_json_value_int(&elem, &i64)
                   && !axl_json_value_uint(&elem, &u64)
                   && !axl_json_value_number_str(&elem, buf, sizeof(buf))
                   && axl_json_value_string(&elem, buf, sizeof(buf))
                   && axl_strcmp(buf, "123") == 0,
                   "value mirror: \"123\" is a STRING — no numeric accessor "
                   "reads it, and the string one does");
        test_check(axl_json_array_next(&it, &elem)
                   && !axl_json_value_int(&elem, &i64),
                   "value mirror: \"1.5\" likewise refuses value_int");
        test_check(axl_json_array_next(&it, &elem)
                   && !axl_json_value_bool(&elem, &b)
                   && axl_json_value_type(&elem) == AXL_JSON_TYPE_STRING,
                   "value mirror: \"true\" is a STRING, not a bool");
        axl_json_free(&r);
    }

    // --- NaN and Infinity type as NUMBER, and null is not confused with them
    // This is what pins the ORDER inside value_type. The number test is
    // positive (first byte is a digit, sign, '.', 'N' or 'I') and must run
    // BEFORE the literal letters; a letter-first order would have to know
    // about NaN/Infinity by name and would silently reclassify whatever is
    // added to the lexer's literal table next. Nothing else in this file
    // reaches those two capitals.
    {
        const char *doc = "[NaN, Infinity, -Infinity, null]";

        test_check(axl_json_parse(doc, axl_strlen(doc),
                                        AXL_JSON_ALLOW_NAN_INF, &r)
                   && axl_json_value_array_begin(&r, &it),
                   "value mirror: a NaN/Infinity array parses under its flag");

        test_check(axl_json_array_next(&it, &elem)
                   && axl_json_value_type(&elem) == AXL_JSON_TYPE_NUMBER
                   && axl_json_value_number_str(&elem, buf, sizeof(buf))
                   && axl_strcmp(buf, "NaN") == 0,
                   "value mirror: NaN is a NUMBER, not an unknown literal");
        test_check(axl_json_array_next(&it, &elem)
                   && axl_json_value_type(&elem) == AXL_JSON_TYPE_NUMBER,
                   "value mirror: Infinity is a NUMBER");
        test_check(axl_json_array_next(&it, &elem)
                   && axl_json_value_type(&elem) == AXL_JSON_TYPE_NUMBER,
                   "value mirror: -Infinity is a NUMBER, sign and all");
        test_check(axl_json_array_next(&it, &elem)
                   && axl_json_value_type(&elem) == AXL_JSON_TYPE_NULL,
                   "value mirror: lowercase null is still NULL beside them — "
                   "the literal table is case-exact");
        axl_json_free(&r);
    }

    // --- an EMPTY array opens true ----------------------------------------
    {
        const char *doc = "[]";

        test_check(axl_json_parse(doc, axl_strlen(doc), AXL_JSON_RELAXED, &r)
                   && axl_json_value_array_begin(&r, &it)
                   && !axl_json_array_next(&it, &elem),
                   "value mirror: an empty array OPENS, then yields nothing — "
                   "false would have meant 'not an array'");
        axl_json_free(&r);
    }

    // --- get_value and get_type -------------------------------------------
    {
        const char *doc = "{\"n\":1,\"s\":\"x\",\"z\":null,\"o\":{},\"a\":[]}";

        test_check(axl_json_parse(doc, axl_strlen(doc), AXL_JSON_RELAXED, &r), "value mirror: "
                   "object parses");

        test_check(axl_json_get_type(&r, "n") == AXL_JSON_TYPE_NUMBER
                   && axl_json_get_type(&r, "s") == AXL_JSON_TYPE_STRING
                   && axl_json_get_type(&r, "z") == AXL_JSON_TYPE_NULL
                   && axl_json_get_type(&r, "o") == AXL_JSON_TYPE_OBJECT
                   && axl_json_get_type(&r, "a") == AXL_JSON_TYPE_ARRAY,
                   "value mirror: get_type names all five types");

        // The distinction the enum is built to preserve.
        test_check(axl_json_get_type(&r, "z") == AXL_JSON_TYPE_NULL
                   && axl_json_get_type(&r, "nope") == AXL_JSON_TYPE_NONE,
                   "value mirror: present-but-null is NOT absent");

        // get_value descends to any type, null included, which is the other
        // way to draw that same line.
        test_check(axl_json_get_value(&r, "z", &elem)
                   && axl_json_value_type(&elem) == AXL_JSON_TYPE_NULL
                   && !axl_json_get_value(&r, "nope", &elem),
                   "value mirror: get_value reaches a null and refuses an "
                   "absent key");

        test_check(axl_json_get_value(&r, "n", &elem)
                   && axl_json_value_int(&elem, &i64) && i64 == 1,
                   "value mirror: get_value + value_int is what get_int is");
        axl_json_free(&r);
    }

    // --- every way to get NONE, including the one that means the opposite --
    {
        const char   *bad = "{oops";
        AxlJsonReader empty;

        axl_memset(&empty, 0, sizeof(empty));
        test_check(axl_json_value_type(NULL) == AXL_JSON_TYPE_NONE
                   && axl_json_get_type(NULL, "k") == AXL_JSON_TYPE_NONE
                   && axl_json_value_type(&empty) == AXL_JSON_TYPE_NONE,
                   "value mirror: NULL and empty readers are NONE, not a "
                   "garbage type");

        test_check(!axl_json_parse(bad, axl_strlen(bad),
                                         AXL_JSON_STRICT, &r)
                   && axl_json_get_type(&r, "anything") == AXL_JSON_TYPE_NONE
                   && axl_json_reader_error(&r)->code != AXL_JSON_OK,
                   "value mirror: a FAILED parse reports NONE for every key, "
                   "and only the error record says so");
        axl_json_free(&r);
    }

    // --- a FALSE return must not have written the out-param ---------------
    // The whole by-key family promises "untouched on false", and the refactor
    // that routed get_object through get_value broke it for that one member:
    // get_value borrows as soon as the KEY exists, so a key holding the wrong
    // type left `out` already overwritten. It licenses this idiom, which
    // silently retargeted at the wrong node:
    //
    //     AxlJsonReader cfg = root;            // default: look in the root
    //     axl_json_get_object(&root, "tls", &cfg);   // narrow IF present
    //     axl_json_get_int(&cfg, "port", &port);
    {
        const char *doc = "{\"o\":{\"port\":8080},\"s\":\"str\",\"n\":1}";
        AxlJsonReader keep;

        test_check(axl_json_parse(doc, axl_strlen(doc), AXL_JSON_RELAXED, &r), "value mirror: "
                   "out-param document parses");

        test_check(axl_json_get_object(&r, "o", &keep)
                   && axl_json_get_int(&keep, "port", &i64) && i64 == 8080,
                   "value mirror: get_object narrows to the nested object");

        /* Each of these must FAIL and leave `keep` pointing where it was. */
        test_check(!axl_json_get_object(&r, "s", &keep)
                   && axl_json_get_int(&keep, "port", &i64) && i64 == 8080,
                   "value mirror: a key holding a STRING leaves out untouched");
        test_check(!axl_json_get_object(&r, "n", &keep)
                   && axl_json_get_int(&keep, "port", &i64) && i64 == 8080,
                   "value mirror: a key holding a NUMBER leaves out untouched");
        test_check(!axl_json_get_object(&r, "absent", &keep)
                   && axl_json_get_int(&keep, "port", &i64) && i64 == 8080,
                   "value mirror: an absent key leaves out untouched");
        axl_json_free(&r);
    }

    // --- argument paths ----------------------------------------------------
    {
        const char *doc = "{\"k\":1}";

        test_check(axl_json_parse(doc, axl_strlen(doc), AXL_JSON_RELAXED, &r), "value mirror: "
                   "arg-path document parses");

        /* get_type on a reader whose own value is not an object: one of the
           five documented NONE causes, and the only one with no other test. */
        {
            const char   *arr = "[1,2,3]";
            AxlJsonReader ar;

            test_check(axl_json_parse(arr, axl_strlen(arr), AXL_JSON_RELAXED, &ar)
                       && axl_json_get_type(&ar, "k") == AXL_JSON_TYPE_NONE
                       && !axl_json_get_value(&ar, "k", &elem),
                       "value mirror: looking a key up in a non-object is "
                       "NONE, not a crash");
            axl_json_free(&ar);
        }

        test_check(!axl_json_value_array_begin(&r, NULL),
                   "value mirror: value_array_begin refuses a NULL iterator");
        test_check(!axl_json_get_value(&r, NULL, &elem)
                   && !axl_json_get_value(&r, "k", NULL)
                   && !axl_json_get_value(NULL, "k", &elem),
                   "value mirror: get_value refuses NULL arguments — a NULL "
                   "key never means 'my own value'");
        test_check(!axl_json_value_int(NULL, &i64)
                   && !axl_json_value_uint(NULL, &u64)
                   && !axl_json_value_bool(NULL, &b)
                   && !axl_json_value_number_str(NULL, buf, sizeof(buf))
                   && !axl_json_value_array_begin(NULL, &it),
                   "value mirror: the own-value family is NULL-safe");

        /* A sub-reader owns nothing, so freeing one must be a no-op that
           leaves the parent usable — the contract that stops an implementer
           copying owns_json into it and double-freeing. */
        test_check(axl_json_get_value(&r, "k", &elem), "value mirror: "
                   "sub-reader taken");
        axl_json_free(&elem);
        test_check(axl_json_get_int(&r, "k", &i64) && i64 == 1,
                   "value mirror: freeing a sub-reader does not disturb the "
                   "parent");
        axl_json_free(&r);
    }
}

// ---------------------------------------------------------------------------
// Object iteration (P11) — the only way to ask what keys an object HAS
// ---------------------------------------------------------------------------

static void
test_json_object_iter(void)
{
    AxlJsonReader     r, val, sub;
    AxlJsonObjectIter it;
    char              key[32];
    int64_t           i64;

    // --- document order, mixed value types --------------------------------
    {
        /* The container sits in the MIDDLE on purpose. With it last,
           `remaining` ends the walk whatever `pos` is, so deleting the
           nested-child skip loop entirely still produced a, b, c -- the
           assertion below read as coverage it did not have. */
        const char *doc = "{\"a\":1,\"c\":[7,8],\"b\":\"x\"}";

        test_check(axl_json_parse(doc, axl_strlen(doc), AXL_JSON_RELAXED, &r)
                   && axl_json_value_object_begin(&r, &it),
                   "obj iter: an object opens");

        test_check(axl_json_object_next(&it, key, sizeof(key), &val)
                   && axl_strcmp(key, "a") == 0
                   && axl_json_value_int(&val, &i64) && i64 == 1,
                   "obj iter: first pair is a=1");
        test_check(axl_json_object_next(&it, key, sizeof(key), &val)
                   && axl_strcmp(key, "c") == 0
                   && axl_json_value_type(&val) == AXL_JSON_TYPE_ARRAY,
                   "obj iter: second pair is c, an array");
        test_check(axl_json_object_next(&it, key, sizeof(key), &val)
                   && axl_strcmp(key, "b") == 0
                   && axl_json_value_type(&val) == AXL_JSON_TYPE_STRING,
                   "obj iter: third pair is b — the array's elements did not "
                   "leak into the walk as pairs of their own");
        test_check(!axl_json_object_next(&it, key, sizeof(key), &val),
                   "obj iter: exactly three pairs");
        axl_json_free(&r);
    }

    // --- the key is DECODED, not a raw view -------------------------------
    // A borrowed raw view would reproduce the \uXXXX corruption phase A
    // existed to fix, one layer up: this key is spelled with an escape and
    // its NAME is "A".
    {
        const char *doc = "{\"\\u0041\":1,\"b\\u0000c\":2}";

        test_check(axl_json_parse(doc, axl_strlen(doc), AXL_JSON_RELAXED, &r)
                   && axl_json_value_object_begin(&r, &it),
                   "obj iter: escaped-key object opens");
        test_check(axl_json_object_next(&it, key, sizeof(key), &val)
                   && axl_strcmp(key, "A") == 0
                   && axl_json_value_int(&val, &i64) && i64 == 1,
                   "obj iter: \\u0041 decodes to the key A");
        test_check(axl_json_object_next(&it, key, sizeof(key), &val)
                   && axl_strcmp(key, "b\xEF\xBF\xBD" "c") == 0,
                   "obj iter: an escaped NUL in a key becomes U+FFFD, as it "
                   "does in a value — the key decoder is the same one");
        axl_json_free(&r);
    }

    // --- JSON5 unquoted and single-quoted keys ----------------------------
    // Not an opt-in corner: axl_json_parse is RELAXED, so this is the DEFAULT
    // dialect. The key token brackets the bare identifier with no quotes to
    // strip, which the decoder has to handle unchanged.
    {
        const char *doc = "{a:1,'b c':2,'d\\'e':3}";
        int64_t     v1 = 0, v2 = 0, v3 = 0;

        test_check(axl_json_parse(doc, axl_strlen(doc), AXL_JSON_RELAXED, &r)
                   && axl_json_value_object_begin(&r, &it),
                   "obj iter: a JSON5 object opens");
        test_check(axl_json_object_next(&it, key, sizeof(key), &val)
                   && axl_strcmp(key, "a") == 0
                   && axl_json_value_int(&val, &v1) && v1 == 1,
                   "obj iter: an UNQUOTED JSON5 key reads as its identifier");
        test_check(axl_json_object_next(&it, key, sizeof(key), &val)
                   && axl_strcmp(key, "b c") == 0
                   && axl_json_value_int(&val, &v2) && v2 == 2,
                   "obj iter: a single-quoted key reads without its quotes");
        test_check(axl_json_object_next(&it, key, sizeof(key), &val)
                   && axl_strcmp(key, "d'e") == 0
                   && axl_json_value_int(&val, &v3) && v3 == 3,
                   "obj iter: and an escaped quote inside one decodes");
        axl_json_free(&r);
    }

    // --- duplicate keys are yielded separately -----------------------------
    {
        const char *doc = "{\"a\":1,\"a\":2}";
        int64_t     first = 0, second = 0;

        test_check(axl_json_parse(doc, axl_strlen(doc), AXL_JSON_RELAXED, &r)
                   && axl_json_value_object_begin(&r, &it)
                   && axl_json_object_next(&it, key, sizeof(key), &val)
                   && axl_json_value_int(&val, &first)
                   && axl_json_object_next(&it, key, sizeof(key), &val)
                   && axl_json_value_int(&val, &second)
                   && !axl_json_object_next(&it, key, sizeof(key), &val),
                   "obj iter: a duplicate key yields TWO pairs, not one");
        test_check(first == 1 && second == 2,
                   "obj iter: and both values arrive, in document order");
        axl_json_free(&r);
    }

    // --- an EMPTY object opens true ----------------------------------------
    {
        const char *doc = "{}";

        test_check(axl_json_parse(doc, axl_strlen(doc), AXL_JSON_RELAXED, &r)
                   && axl_json_value_object_begin(&r, &it)
                   && !axl_json_object_next(&it, key, sizeof(key), &val),
                   "obj iter: an empty object OPENS, then yields nothing");
        axl_json_free(&r);
    }

    // --- by key, and over a nested object ---------------------------------
    {
        const char *doc = "{\"outer\":{\"p\":10,\"q\":20},\"other\":1}";
        int64_t     sum = 0;

        test_check(axl_json_parse(doc, axl_strlen(doc), AXL_JSON_RELAXED, &r)
                   && axl_json_object_begin(&r, "outer", &it),
                   "obj iter: object_begin descends by key");
        while (axl_json_object_next(&it, key, sizeof(key), &val)) {
            if (axl_json_value_int(&val, &i64)) {
                sum += i64;
            }
        }
        test_check(sum == 30,
                   "obj iter: the nested object's two members are walked, and "
                   "the sibling key is not");
        axl_json_free(&r);
    }

    // --- NULL key_buf walks values only ------------------------------------
    {
        const char *doc = "{\"a\":1,\"b\":2}";
        int64_t     sum = 0;

        test_check(axl_json_parse(doc, axl_strlen(doc), AXL_JSON_RELAXED, &r)
                   && axl_json_value_object_begin(&r, &it),
                   "obj iter: values-only object opens");
        while (axl_json_object_next(&it, NULL, 0, &val)) {
            if (axl_json_value_int(&val, &i64)) {
                sum += i64;
            }
        }
        test_check(sum == 3,
                   "obj iter: a NULL key buffer walks the values and skips "
                   "the keys");
        axl_json_free(&r);
    }

    // --- a key too long is TRUNCATED, reported, and the walk continues -----
    // Ending iteration over one oversized key would lose every later pair, so
    // the pair is yielded — but a prefix cannot be recognised as short from
    // its own contents, so the caller is TOLD.
    {
        const char *doc = "{\"averylongkeyname\":1,\"z\":2}";
        char        small[8];   /* 7 chars + NUL */
        int64_t     zval = 0;

        test_check(axl_json_parse(doc, axl_strlen(doc), AXL_JSON_RELAXED, &r)
                   && axl_json_value_object_begin(&r, &it),
                   "obj iter: long-key object opens");
        test_check(axl_json_object_next(&it, small, sizeof(small), &val)
                   && axl_json_object_iter_error(&it)->code
                      == AXL_JSON_ERR_TRUNCATED
                   && axl_json_value_int(&val, &i64) && i64 == 1,
                   "obj iter: an oversized key reports TRUNCATED and the pair "
                   "is still yielded");
        test_check(axl_json_object_next(&it, small, sizeof(small), &val)
                   && axl_strcmp(small, "z") == 0
                   && axl_json_object_iter_error(&it)->code == AXL_JSON_OK
                   && axl_json_value_int(&val, &zval) && zval == 2,
                   "obj iter: the walk continues, and the code RESETS on a "
                   "key that fits — it is per-pair, not sticky");
        axl_json_free(&r);
    }

    // --- the false match the report exists to prevent ----------------------
    // A multi-byte character is refused WHOLE, so a truncation can land
    // several bytes short of the buffer. "userés" in a 6-byte buffer decodes
    // to exactly "user" — indistinguishable from the real key "user" by
    // content alone. Measured before the fix: 700 of 1000 generated keys
    // collided with a 4-byte target at the buffer size the docstring then
    // claimed was safe.
    {
        const char *doc = "{\"user\\u00e9s\":1}";
        char        small[6];

        test_check(axl_json_parse(doc, axl_strlen(doc), AXL_JSON_RELAXED, &r)
                   && axl_json_value_object_begin(&r, &it)
                   && axl_json_object_next(&it, small, sizeof(small), &val),
                   "obj iter: multi-byte-key object yields its pair");
        test_check(axl_strcmp(small, "user") == 0,
                   "obj iter: the truncated key is byte-identical to a "
                   "DIFFERENT real key — content alone cannot tell");
        test_check(axl_json_object_iter_error(&it)->code
                   == AXL_JSON_ERR_TRUNCATED,
                   "obj iter: and only the reported code distinguishes them");
        axl_json_free(&r);
    }

    // --- a key discovered by iteration ROUND-TRIPS into the by-key family --
    // token_equals compared RAW source bytes while object_next decodes, so an
    // escaped key was findable only by its raw spelling — and a plain RFC 8259
    // \t was enough. Object iteration is what made that contradiction live:
    // it is the only way to LEARN a key, and the obvious next move is to
    // use one.
    {
        const char *doc = "{\"\\u0041\":1,\"tab\\tkey\":2,\"plain\":3}";
        int         round = 0;

        test_check(axl_json_parse(doc, axl_strlen(doc), AXL_JSON_RELAXED, &r)
                   && axl_json_value_object_begin(&r, &it),
                   "obj iter: escaped-key round-trip object opens");
        while (axl_json_object_next(&it, key, sizeof(key), &val)) {
            AxlJsonReader back;

            if (axl_json_object_iter_error(&it)->code == AXL_JSON_OK
                && axl_json_get_value(&r, key, &back)
                && axl_json_value_int(&back, &i64)) {
                round++;
            }
        }
        test_check(round == 3,
                   "obj iter: every discovered key finds its own value again "
                   "through get_value — decoded names, both directions");
        axl_json_free(&r);
    }

    // --- and the by-key family answers to the DECODED name -----------------
    {
        const char *doc = "{\"a\\u0042c\":1,\"tab\\tkey\":2}";

        test_check(axl_json_parse(doc, axl_strlen(doc), AXL_JSON_RELAXED, &r), "obj iter: "
                   "escaped-key lookup document parses");
        test_check(axl_json_get_int(&r, "aBc", &i64) && i64 == 1,
                   "obj iter: {\"a\\u0042c\":1} is found by the name aBc");
        test_check(axl_json_get_int(&r, "tab\tkey", &i64) && i64 == 2,
                   "obj iter: a plain RFC 8259 \\t in a key is findable too");
        test_check(!axl_json_get_int(&r, "a\\u0042c", &i64),
                   "obj iter: and NOT by its raw source spelling");
        axl_json_free(&r);
    }

    // --- reusing the VALUE reader must not retarget the iterator ----------
    // The reason this type holds the document by value. AxlJsonArrayIter had
    // to be FIXED into that shape; this one was written from it.
    {
        const char       *doc = "{\"a\":{\"n\":1},\"b\":{\"n\":2}}";
        AxlJsonObjectIter inner;

        test_check(axl_json_parse(doc, axl_strlen(doc), AXL_JSON_RELAXED, &r)
                   && axl_json_value_object_begin(&r, &it)
                   && axl_json_object_next(&it, key, sizeof(key), &val),
                   "obj iter: alias setup — first pair taken");
        test_check(axl_json_value_object_begin(&val, &inner),
                   "obj iter: an inner iterator opens on that value");
        test_check(axl_json_object_next(&it, key, sizeof(key), &val)
                   && axl_strcmp(key, "b") == 0,
                   "obj iter: the outer walk reuses the value reader");
        test_check(axl_json_object_next(&inner, key, sizeof(key), &sub)
                   && axl_strcmp(key, "n") == 0
                   && axl_json_value_int(&sub, &i64) && i64 == 1,
                   "obj iter: the inner iterator still yields ITS object's "
                   "member — value 1, not the reused reader's 2");
        axl_json_free(&r);
    }

    // --- argument paths ----------------------------------------------------
    {
        const char   *doc = "{\"a\":1}";
        const char   *arr = "[1,2]";
        AxlJsonReader ar;

        test_check(axl_json_parse(doc, axl_strlen(doc), AXL_JSON_RELAXED, &r), "obj iter: "
                   "arg-path document parses");
        test_check(!axl_json_value_object_begin(&r, NULL)
                   && !axl_json_value_object_begin(NULL, &it)
                   && !axl_json_object_begin(&r, NULL, &it)
                   && !axl_json_object_begin(&r, "a", &it),
                   "obj iter: NULL args are refused, and so is a key whose "
                   "value is not an object");

        test_check(axl_json_parse(arr, axl_strlen(arr), AXL_JSON_RELAXED, &ar)
                   && !axl_json_value_object_begin(&ar, &it),
                   "obj iter: an ARRAY is not an object");
        axl_json_free(&ar);

        test_check(!axl_json_object_begin(&r, "nosuchkey", &it),
                   "obj iter: object_begin refuses an ABSENT key, distinctly "
                   "from a key of the wrong type");

        /* iter untouched on false, the same promise get_object makes. */
        {
            AxlJsonObjectIter keep;
            AxlJsonReader     kv;

            test_check(axl_json_value_object_begin(&r, &keep)
                       && !axl_json_object_begin(&r, "nosuchkey", &keep)
                       && !axl_json_value_object_begin(&ar, &keep)
                       && axl_json_object_next(&keep, key, sizeof(key), &kv)
                       && axl_strcmp(key, "a") == 0,
                       "obj iter: a failed begin leaves the iterator "
                       "untouched and still walkable");
        }

        test_check(axl_json_value_object_begin(&r, &it)
                   && !axl_json_object_next(&it, key, sizeof(key), NULL)
                   && !axl_json_object_next(NULL, key, sizeof(key), &val),
                   "obj iter: next refuses a NULL value out-param or iterator");
        axl_json_free(&r);
    }
}

// ---------------------------------------------------------------------------
// The public string decoder (P11) — the inverse of axl_json_escape_string
// ---------------------------------------------------------------------------

static void
test_json_decode_string(void)
{
    char buf[64];

    // --- the escape set, including the JSON5 superset ----------------------
    {
        struct { const char *src; const char *want; const char *msg; } row[] = {
            { "plain", "plain", "decode: an escape-free string is itself" },
            { "a\\u0041b", "aAb", "decode: \\uXXXX resolves" },
            { "\\ud83d\\ude00", "\xF0\x9F\x98\x80",
              "decode: a surrogate PAIR combines into one 4-byte code point, "
              "not two 3-byte sequences" },
            { "\\ud83d", "\xEF\xBF\xBD",
              "decode: a LONE surrogate becomes U+FFFD" },
            { "a\\u0000b", "a\xEF\xBF\xBD" "b",
              "decode: an escaped NUL becomes U+FFFD, never an interior NUL" },
            { "a\\0b", "a\xEF\xBF\xBD" "b",
              "decode: JSON5 \\0 likewise" },
            { "a\\x00b", "a\xEF\xBF\xBD" "b",
              "decode: and JSON5 \\x00" },
            { "\\x41", "A", "decode: \\xNN is the code unit U+00NN" },
            { "q\\\"q", "q\"q", "decode: an escaped quote" },
            { "t\\tb", "t\tb", "decode: the RFC 8259 whitespace escapes" },
            { "s\\'q", "s'q", "decode: JSON5 \\' " },
        };
        size_t i;

        for (i = 0; i < sizeof(row) / sizeof(row[0]); i++) {
            const int n = axl_json_decode_string(row[i].src,
                                                 axl_strlen(row[i].src),
                                                 buf, sizeof(buf));
            test_check(n > 0 && axl_strcmp(buf, row[i].want) == 0
                       && (size_t)n == axl_strlen(row[i].want),
                       row[i].msg);
        }
    }

    // --- it is the inverse of the encoder, across the quotes ---------------
    {
        const char *original = "he said \"hi\"\n\tand left";
        char        enc[128];
        const int   e = axl_json_escape_string(original, enc, sizeof(enc));

        /* escape_string writes WITH quotes; the decoder takes the inner form,
           so the round trip skips the opening quote and drops two. */
        test_check(e > 2 && enc[0] == '"' && enc[e - 1] == '"',
                   "decode: the encoder brackets its output in quotes");
        const int d = axl_json_decode_string(enc + 1, (size_t)e - 2,
                                             buf, sizeof(buf));
        test_check(d > 0 && axl_strcmp(buf, original) == 0,
                   "decode: escape then decode reproduces the original "
                   "exactly");
    }

    // --- truncation is REFUSED, unlike axl_json_get_string -----------------
    // A caller here has no reader to interrogate, so a silent prefix would be
    // a value that compares equal to the wrong thing.
    {
        const char *src = "abcdefgh";
        char        small[4];

        axl_memset(small, (char)0xAA, sizeof(small));
        test_check(axl_json_decode_string(src, axl_strlen(src),
                                          small, sizeof(small)) == -1,
                   "decode: a result too long for the buffer returns -1 "
                   "rather than a prefix");

        /* A multi-byte unit is refused WHOLE — the case where a length check
           alone cannot detect the shortfall. `é` is two bytes, so a 3-byte
           buffer holds it exactly and a 2-byte one cannot hold it at all;
           both are asserted, because the boundary is the whole point. */
        test_check(axl_json_decode_string("\\u00e9", 6, small, 3) == 2
                   && axl_strcmp(small, "\xC3\xA9") == 0,
                   "decode: a 2-byte code point fits a 3-byte buffer exactly");
        test_check(axl_json_decode_string("\\u00e9", 6, small, 2) == -1,
                   "decode: and is refused whole by a 2-byte one, never split "
                   "into half a sequence");
    }

    // --- exact fit, and the empty string -----------------------------------
    {
        test_check(axl_json_decode_string("abc", 3, buf, 4) == 3
                   && axl_strcmp(buf, "abc") == 0,
                   "decode: len+1 is enough for an ESCAPE-FREE string");
        test_check(axl_json_decode_string("", 0, buf, sizeof(buf)) == 0
                   && buf[0] == '\0',
                   "decode: an empty string decodes to an empty string");
    }

    // --- a decode CAN grow, so len+1 is not a sufficient bound -------------
    // JSON5 `\0` is the one escape that expands: two source bytes in, the
    // three bytes of U+FFFD out. Every other form shrinks or holds. The
    // docstring used to promise "a decode never grows a string, so len + 1
    // always suffices" and a caller who believed it got a spurious -1 on
    // input it had sized correctly by the documented rule.
    //
    // The escape-set table above already decodes `a\0b` -- 4 bytes in, 5 out
    // -- and could not catch this because it decodes into a 64-byte buffer.
    // Growth was visible in the DATA and invisible to the ASSERTION.
    {
        char tight[4];

        test_check(axl_json_decode_string("\\0", 2, tight, 3) == -1,
                   "decode: len+1 is REFUSED for \\0, which grows 2 bytes "
                   "into 3");
        test_check(axl_json_decode_string("\\0", 2, tight, 4) == 3
                   && axl_strcmp(tight, "\xEF\xBF\xBD") == 0,
                   "decode: the documented len*3/2+1 bound holds it exactly");
    }

    // --- sweep the bound, at BOTH parities ---------------------------------
    // A hand-picked length lands wherever it lands. Two things have to be
    // swept: the ratio only bites when `\0` is DENSE, and `len * 3 / 2` only
    // TRUNCATES when len is odd -- so an all-pairs sweep, which is necessarily
    // even, never exercises the formula's riskiest arithmetic at all.
    //
    // The densest input at each length is floor(len/2) `\0` pairs plus, when
    // the length is odd, one 1:1 filler byte.
    {
        enum { SWEEP_PAIRS = 12 };
        char   src[SWEEP_PAIRS * 2 + 1];
        char   out[SWEEP_PAIRS * 3 + 2];
        char   want[SWEEP_PAIRS * 3 + 2];
        size_t len;
        bool   all_fit   = true;
        bool   all_exact = true;

        for (len = 1; len <= SWEEP_PAIRS * 2; len++) {
            const size_t pairs = len / 2;
            const size_t bound = len * 3 / 2 + 1;
            size_t       i;
            size_t       w = 0;
            int          n;

            for (i = 0; i < pairs; i++) {
                src[i * 2]     = '\\';
                src[i * 2 + 1] = '0';
                want[w++]      = (char)0xEF;
                want[w++]      = (char)0xBF;
                want[w++]      = (char)0xBD;
            }
            if (len % 2 != 0) {
                src[len - 1] = 'z';
                want[w++]    = 'z';
            }
            want[w] = '\0';

            /* The BYTES, not just the length: three arbitrary bytes per `\0`
               would satisfy a length-only check while decoding wrongly. */
            n = axl_json_decode_string(src, len, out, bound);
            if (n != (int)w || axl_strcmp(out, want) != 0) {
                all_fit = false;
            }
            /* And one byte less must fail, or the bound is loose rather than
               tight -- a loose bound would let this pass while still
               mis-documenting the real requirement. */
            if (axl_json_decode_string(src, len, out, bound - 1) != -1) {
                all_exact = false;
            }
        }
        test_check(all_fit,
                   "decode: len*3/2+1 holds the densest input at every length "
                   "1..24, bytes and all");
        test_check(all_exact,
                   "decode: and is TIGHT -- one byte less is refused at every "
                   "one of them, odd lengths included");
    }

    // --- argument paths ----------------------------------------------------
    {
        test_check(axl_json_decode_string(NULL, 3, buf, sizeof(buf)) == -1
                   && axl_json_decode_string("abc", 3, NULL, sizeof(buf)) == -1
                   && axl_json_decode_string("abc", 3, buf, 0) == -1,
                   "decode: NULL arguments and a zero-sized buffer are "
                   "refused");
    }
}

// ---------------------------------------------------------------------------
// Writer formatting (P5) — COMPACT, ESCAPE_SLASH, EMBED
//
// Exact WHOLE-DOCUMENT compares throughout, per the workflow's output rule: a
// substring match would let most of the regressions these guard through.
// ---------------------------------------------------------------------------

/* One nested document, built identically under every flag set, so the flags
   are compared against each OTHER and not each against its own expectation. */
static void
build_fmt_doc(AxlJsonWriter *w)
{
    axl_json_obj_begin(w);
    axl_json_kv_int(w, "a", 1);
    axl_json_key(w, "o");
    axl_json_obj_begin(w);
    axl_json_kv_str(w, "p", "x/y");
    axl_json_key(w, "d");
    axl_json_obj_begin(w);
    axl_json_kv_int(w, "z", 2);
    axl_json_obj_end(w);
    axl_json_obj_end(w);
    axl_json_obj_end(w);
}

/* An ARRAY root, so the EMBED identity is asserted for both delimiters. It
   was previously checked only for `{}`, with the bracket case a separate
   hardcoded string outside the loop. */
static void
build_fmt_arr(AxlJsonWriter *w)
{
    axl_json_arr_begin(w);
    axl_json_int(w, 1);
    axl_json_arr_begin(w);
    axl_json_str(w, "n/a");
    axl_json_arr_end(w);
    axl_json_arr_end(w);
}

/* EMBED's defining identity: wrapping the embedded output in the delimiter it
   omitted must reproduce the unembedded output byte for byte. Asserted per
   flag set rather than folded into one boolean, so a failure names the
   setting that broke rather than only that something did. */
static void
fmt_embed_identity(void (*build)(AxlJsonWriter *), AxlJsonFlags flags,
                   char open_ch, char close_ch, const char *msg)
{
    AXL_AUTOPTR(AxlString) plain = axl_string_new(NULL);
    AXL_AUTOPTR(AxlString) emb   = axl_string_new(NULL);
    AXL_AUTOPTR(AxlString) wrap  = axl_string_new(NULL);
    AxlJsonWriter          w;

    axl_json_writer_init(&w, plain, flags);
    build(&w);
    axl_json_writer_finish(&w);

    axl_json_writer_init(&w, emb, flags | AXL_JSON_EMBED);
    build(&w);
    axl_json_writer_finish(&w);

    axl_string_append_c(wrap, open_ch);
    axl_string_append(wrap, axl_string_str(emb));
    axl_string_append_c(wrap, close_ch);

    test_check(axl_strcmp(axl_string_str(wrap), axl_string_str(plain)) == 0,
               msg);
}

static void
fmt_check(AxlJsonFlags flags, const char *want, const char *msg)
{
    AXL_AUTOPTR(AxlString) out = axl_string_new(NULL);
    AxlJsonWriter          w;

    axl_json_writer_init(&w, out, flags);
    build_fmt_doc(&w);
    axl_json_writer_finish(&w);
    test_check(!axl_json_writer_error(&w)
               && axl_strcmp(axl_string_str(out), want) == 0, msg);
}

static void
test_json_writer_format(void)
{
    // --- the baseline, and the presence bit ------------------------------
    // INDENT(0) must NOT equal "no indent flag": newlines with zero indent
    // versus fully compact. If they match, the presence bit is broken — which
    // is the whole reason it exists.
    fmt_check(AXL_JSON_STRICT,
              "{\"a\":1,\"o\":{\"p\":\"x/y\",\"d\":{\"z\":2}}}",
              "fmt: no indent flag is fully compact");
    fmt_check(AXL_JSON_INDENT(0),
              "{\n\"a\": 1,\n\"o\": {\n\"p\": \"x/y\",\n\"d\": {\n\"z\": 2\n}\n}\n}",
              "fmt: INDENT(0) is newlines with ZERO indent, not compact — the "
              "presence bit is what tells them apart");
    fmt_check(AXL_JSON_INDENT(2),
              "{\n  \"a\": 1,\n  \"o\": {\n    \"p\": \"x/y\",\n    \"d\": {\n      \"z\": 2\n    }\n  }\n}",
              "fmt: INDENT(2) indents each level by two");
    fmt_check(AXL_JSON_INDENT(1),
              "{\n \"a\": 1,\n \"o\": {\n  \"p\": \"x/y\",\n  \"d\": {\n"
              "   \"z\": 2\n  }\n }\n}",
              "fmt: INDENT(1) — one space per level, three levels deep");
    fmt_check(AXL_JSON_INDENT(8),
              "{\n        \"a\": 1,\n        \"o\": {\n"
              "                \"p\": \"x/y\",\n                \"d\": {\n"
              "                        \"z\": 2\n                }\n"
              "        }\n}",
              "fmt: INDENT(8) honours its width at every depth");

    // --- COMPACT ----------------------------------------------------------
    fmt_check(AXL_JSON_STRICT | AXL_JSON_COMPACT,
              "{\"a\":1,\"o\":{\"p\":\"x/y\",\"d\":{\"z\":2}}}",
              "fmt: COMPACT alone changes nothing — AXL's unindented output "
              "is already compact, unlike Jansson's");
    fmt_check(AXL_JSON_INDENT(2) | AXL_JSON_COMPACT,
              "{\n  \"a\":1,\n  \"o\":{\n    \"p\":\"x/y\",\n    \"d\":{\n      \"z\":2\n    }\n  }\n}",
              "fmt: INDENT(2) | COMPACT keeps the newlines and drops the "
              "space after the colon");

    // --- ESCAPE_SLASH -----------------------------------------------------
    fmt_check(AXL_JSON_STRICT | AXL_JSON_ESCAPE_SLASH,
              "{\"a\":1,\"o\":{\"p\":\"x\\/y\",\"d\":{\"z\":2}}}",
              "fmt: ESCAPE_SLASH writes / as \\/ in a value");
    {
        AXL_AUTOPTR(AxlString) out = axl_string_new(NULL);
        AxlJsonWriter          w;

        /* Keys too, not just values — they go through the same quoting path
           and a fix applied to one is easy to forget on the other. */
        axl_json_writer_init(&w, out, AXL_JSON_ESCAPE_SLASH);
        axl_json_obj_begin(&w);
        axl_json_kv_int(&w, "a/b", 1);
        axl_json_obj_end(&w);
        axl_json_writer_finish(&w);
        test_check(axl_strcmp(axl_string_str(out), "{\"a\\/b\":1}") == 0,
                   "fmt: ESCAPE_SLASH applies to KEYS as well as values");
    }

    // --- EMBED, and the identity that defines it --------------------------
    fmt_check(AXL_JSON_STRICT | AXL_JSON_EMBED,
              "\"a\":1,\"o\":{\"p\":\"x/y\",\"d\":{\"z\":2}}",
              "fmt: EMBED drops the outermost braces and nothing else — the "
              "NESTED object keeps its own");
    fmt_check(AXL_JSON_INDENT(2) | AXL_JSON_EMBED,
              "\n  \"a\": 1,\n  \"o\": {\n    \"p\": \"x/y\",\n    \"d\": {\n      \"z\": 2\n    }\n  }\n",
              "fmt: EMBED | INDENT(2) keeps every member's indentation, and "
              "the newlines that belong to the members rather than the braces");

    // The contract stated as an executable identity, per setting and for BOTH
    // root delimiters.
    fmt_embed_identity(build_fmt_doc, AXL_JSON_STRICT, '{', '}',
                       "fmt: EMBED identity holds for an object root, compact");
    fmt_embed_identity(build_fmt_doc, AXL_JSON_INDENT(0), '{', '}',
                       "fmt: EMBED identity holds at INDENT(0)");
    fmt_embed_identity(build_fmt_doc, AXL_JSON_INDENT(2), '{', '}',
                       "fmt: EMBED identity holds at INDENT(2)");
    fmt_embed_identity(build_fmt_doc, AXL_JSON_INDENT(8), '{', '}',
                       "fmt: EMBED identity holds at INDENT(8)");
    fmt_embed_identity(build_fmt_doc, AXL_JSON_INDENT(2) | AXL_JSON_COMPACT,
                       '{', '}',
                       "fmt: EMBED identity holds at INDENT(2) | COMPACT");
    fmt_embed_identity(build_fmt_doc,
                       AXL_JSON_RELAXED | AXL_JSON_ALLOW_TRAILING_COMMA,
                       '{', '}',
                       "fmt: EMBED identity holds with a TRAILING COMMA");
    fmt_embed_identity(build_fmt_arr, AXL_JSON_STRICT, '[', ']',
                       "fmt: EMBED identity holds for an ARRAY root, compact");
    fmt_embed_identity(build_fmt_arr, AXL_JSON_INDENT(2), '[', ']',
                       "fmt: EMBED identity holds for an ARRAY root, indented");

    // --- ESCAPE_SLASH on the SPLICE path ----------------------------------
    // The flag's stated purpose is embedding in a <script> block, where `</`
    // closes the element early. "Parse a document I do not control, re-emit
    // it into a page" is exactly that use case — and axl_json_write_token was
    // the one path where the flag did nothing, in keys and values alike.
    //
    // An already-escaped `\/` in the source must stay ONE escape, not become
    // `\\/`: the splice sees source-form bytes, so the backslash and the byte
    // after it travel together.
    {
        const char            *doc = "{\"k/1\":\"a/b\",\"e\":\"x\\/y\"}";
        AXL_AUTOPTR(AxlString) out = axl_string_new(NULL);
        AxlJsonReader          r;
        AxlJsonWriter          w;

        test_check(axl_json_parse(doc, axl_strlen(doc), AXL_JSON_RELAXED, &r),
                   "fmt: splice source parses");
        axl_json_writer_init(&w, out, AXL_JSON_ESCAPE_SLASH);
        axl_json_write_token(&w, &r, 0);
        axl_json_writer_finish(&w);
        test_check(axl_strcmp(axl_string_str(out),
                              "{\"k\\/1\":\"a\\/b\",\"e\":\"x\\/y\"}") == 0,
                   "fmt: ESCAPE_SLASH reaches write_token — keys and values "
                   "both, and an existing \\/ is not double-escaped");
        axl_json_free(&r);
    }

    // --- a COMMENT body is not string content ------------------------------
    // Exact whole-document compare, not a substring probe: a substring match
    // would still pass if the rest of the document were mangled, which is the
    // regression this is meant to catch.
    {
        AXL_AUTOPTR(AxlString) out = axl_string_new(NULL);
        AxlJsonWriter          w;

        /* ESCAPE_SLASH alone, NOT via AXL_JSON_RELAXED: that preset carries
           AXL_JSON_ALLOW_TRAILING_COMMA, which the writer honours, so the
           document would end `,}` and the comparison would be about two
           things at once. */
        axl_json_writer_init(&w, out, AXL_JSON_ESCAPE_SLASH);
        axl_json_obj_begin(&w);
        axl_json_comment(&w, "see http://example.com");
        axl_json_kv_str(&w, "u", "http://example.com");
        axl_json_obj_end(&w);
        axl_json_writer_finish(&w);
        test_check(axl_strcmp(axl_string_str(out),
                              "{/* see http://example.com */"
                              "\"u\":\"http:\\/\\/example.com\"}") == 0,
                   "fmt: ESCAPE_SLASH escapes a VALUE's slashes and leaves a "
                   "comment body alone — comments are not string content");
    }

    // --- an EMBEDded ARRAY root -------------------------------------------
    {
        AXL_AUTOPTR(AxlString) out = axl_string_new(NULL);
        AxlJsonWriter          w;

        axl_json_writer_init(&w, out, AXL_JSON_EMBED);
        axl_json_arr_begin(&w);
        axl_json_int(&w, 1);
        axl_json_int(&w, 2);
        axl_json_arr_end(&w);
        axl_json_writer_finish(&w);
        test_check(axl_strcmp(axl_string_str(out), "1,2") == 0,
                   "fmt: EMBED drops a root ARRAY's brackets too, not just "
                   "an object's braces");
    }
}

// ---------------------------------------------------------------------------
// A comment at depth 0 (regression)
//
// axl_json_comment set needs_comma unconditionally, including at depth 0 where
// there is no sibling to separate. begin_item's "a second value at depth 0 is
// not valid JSON" guard then fired on the very next value, so a FILE-HEADER
// comment — the most common JSON5 comment shape there is — poisoned the
// document: WRITER_STATE, and the output stopped at the comment.
//
// Every pre-existing comment test writes inside a container, which is why
// nothing caught it.
// ---------------------------------------------------------------------------

static void
test_json_comment_depth0(void)
{
    // --- a leading comment, then the document ------------------------------
    {
        AXL_AUTOPTR(AxlString) out = axl_string_new(NULL);
        AxlJsonWriter          w;

        axl_json_writer_init(&w, out, AXL_JSON_STRICT);
        axl_json_comment(&w, "header");
        axl_json_obj_begin(&w);
        axl_json_kv_int(&w, "a", 1);
        axl_json_obj_end(&w);
        axl_json_writer_finish(&w);

        test_check(!axl_json_writer_error(&w)
                   && axl_strcmp(axl_string_str(out),
                                 "/* header */{\"a\":1}") == 0,
                   "comment d0: a file-header comment does not poison the "
                   "document that follows it");
    }

    // --- the same, pretty (the `//` form) ----------------------------------
    {
        AXL_AUTOPTR(AxlString) out = axl_string_new(NULL);
        AxlJsonWriter          w;

        axl_json_writer_init(&w, out, AXL_JSON_INDENT(2));
        axl_json_comment(&w, "header");
        axl_json_obj_begin(&w);
        axl_json_kv_int(&w, "a", 1);
        axl_json_obj_end(&w);
        axl_json_writer_finish(&w);

        test_check(!axl_json_writer_error(&w)
                   && axl_strcmp(axl_string_str(out),
                                 "// header\n{\n  \"a\": 1\n}") == 0,
                   "comment d0: the line-comment form is TERMINATED before "
                   "the value — `// header{` swallowed the brace");

        /* The point is not the bytes, it is that they parse. A `//` comment
           runs to end of line, so the unterminated form produced a document
           whose opening brace was inside the comment. */
        {
            AxlJsonReader rr;
            int64_t       av = 0;

            test_check(axl_json_parse(axl_string_str(out),
                                      axl_strlen(axl_string_str(out)), AXL_JSON_RELAXED, &rr)
                       && axl_json_get_int(&rr, "a", &av) && av == 1,
                       "comment d0: and the result actually PARSES back");
            axl_json_free(&rr);
        }
    }

    // --- but a SECOND root value is still refused --------------------------
    // The reason the fix cannot simply clear needs_comma at depth 0: after the
    // root value it is what makes a second one an error, and a trailing
    // comment must not launder that away.
    {
        AXL_AUTOPTR(AxlString) out = axl_string_new(NULL);
        AxlJsonWriter          w;

        axl_json_writer_init(&w, out, AXL_JSON_STRICT);
        axl_json_obj_begin(&w);
        axl_json_obj_end(&w);
        axl_json_comment(&w, "trailing");
        axl_json_int(&w, 42);
        axl_json_writer_finish(&w);

        test_check(axl_json_writer_error_info(&w)->code
                   == AXL_JSON_ERR_WRITER_STATE,
                   "comment d0: a comment AFTER the root does not license a "
                   "second root value");
    }

    // --- two leading comments ----------------------------------------------
    {
        AXL_AUTOPTR(AxlString) out = axl_string_new(NULL);
        AxlJsonWriter          w;

        axl_json_writer_init(&w, out, AXL_JSON_STRICT);
        axl_json_comment(&w, "one");
        axl_json_comment(&w, "two");
        axl_json_int(&w, 7);
        axl_json_writer_finish(&w);

        test_check(!axl_json_writer_error(&w)
                   && axl_strcmp(axl_string_str(out),
                                 "/* one *//* two */7") == 0,
                   "comment d0: consecutive header comments, then a bare "
                   "root value");
    }

    // --- inside a container, unchanged -------------------------------------
    {
        AXL_AUTOPTR(AxlString) out = axl_string_new(NULL);
        AxlJsonWriter          w;

        axl_json_writer_init(&w, out, AXL_JSON_STRICT);
        axl_json_obj_begin(&w);
        axl_json_kv_int(&w, "a", 1);
        axl_json_comment(&w, "mid");
        axl_json_kv_int(&w, "b", 2);
        axl_json_obj_end(&w);
        axl_json_writer_finish(&w);

        test_check(!axl_json_writer_error(&w)
                   && axl_strcmp(axl_string_str(out),
                                 "{\"a\":1,/* mid */\"b\":2}") == 0,
                   "comment d0: a comment INSIDE a container still separates "
                   "its siblings with exactly one comma");
    }
}

// ---------------------------------------------------------------------------
// Multi-line comment bodies (regression)
//
// comment_body returned at the first newline, so everything after it was
// SILENTLY DROPPED — in both forms. Half-justified: a raw newline really would
// break out of a `//` line comment, but the answer to that is to start a new
// `// ` line, not to discard the text. A `/* */` block has no such hazard at
// all; newlines are legal inside one, and AXL could READ a multi-line block
// comment it could not WRITE.
// ---------------------------------------------------------------------------

/* Build, then assert the exact bytes AND that they parse back — the bytes
   were never the point; a comment that eats the document is. */
static void
comment_check(AxlJsonFlags flags, const char *text, const char *want,
              const char *msg)
{
    AXL_AUTOPTR(AxlString) out = axl_string_new(NULL);
    AxlJsonWriter          w;
    AxlJsonReader          r;
    int64_t                v  = 0;
    bool                   ok;

    axl_json_writer_init(&w, out, flags);
    axl_json_obj_begin(&w);
    axl_json_comment(&w, text);
    axl_json_kv_int(&w, "k", 1);
    axl_json_obj_end(&w);
    axl_json_writer_finish(&w);

    ok = !axl_json_writer_error(&w)
         && axl_strcmp(axl_string_str(out), want) == 0;

    /* Liberal parse: comments need ALLOW_COMMENTS, which axl_json_parse has. */
    ok = ok && axl_json_parse(axl_string_str(out),
                              axl_strlen(axl_string_str(out)), AXL_JSON_RELAXED, &r)
            && axl_json_get_int(&r, "k", &v) && v == 1;
    axl_json_free(&r);
    test_check(ok, msg);
}

static void
test_json_comment_multiline(void)
{
    // --- the BLOCK form keeps its newlines --------------------------------
    comment_check(AXL_JSON_STRICT, "line one\nline two",
                  "{/* line one\nline two */\"k\":1}",
                  "comment ml: a block comment carries newlines through — it "
                  "is what the reader already accepts");

    comment_check(AXL_JSON_STRICT, "one\ntwo\nthree",
                  "{/* one\ntwo\nthree */\"k\":1}",
                  "comment ml: three lines, all of them");

    // --- the LINE form starts a new `// ` per line ------------------------
    comment_check(AXL_JSON_INDENT(2), "line one\nline two",
                  "{\n  // line one\n  // line two\n  \"k\": 1\n}",
                  "comment ml: a line comment continues onto a new `// ` line "
                  "at the same indent, rather than dropping the rest");

    // --- CRLF is ONE line break -------------------------------------------
    comment_check(AXL_JSON_INDENT(2), "one\r\ntwo",
                  "{\n  // one\n  // two\n  \"k\": 1\n}",
                  "comment ml: <CR><LF> is one terminator, not two — no blank "
                  "comment line between them");

    // --- a blank line in the middle survives ------------------------------
    comment_check(AXL_JSON_INDENT(2), "one\n\ntwo",
                  "{\n  // one\n  //\n  // two\n  \"k\": 1\n}",
                  "comment ml: an intentional blank line becomes an empty "
                  "comment line, not a dropped one");

    // --- a TRAILING newline leaves no dangling marker ---------------------
    comment_check(AXL_JSON_INDENT(2), "one\n",
                  "{\n  // one\n  \"k\": 1\n}",
                  "comment ml: a trailing newline does not emit an empty "
                  "`//` with nothing after it");
    comment_check(AXL_JSON_STRICT, "one\n",
                  "{/* one\n */\"k\":1}",
                  "comment ml: the block form keeps a trailing newline, which "
                  "is just a byte inside the comment");

    // --- the close-comment split still applies across lines ---------------
    {
        AXL_AUTOPTR(AxlString) out = axl_string_new(NULL);
        AxlJsonWriter          w;

        axl_json_writer_init(&w, out, AXL_JSON_STRICT);
        axl_json_obj_begin(&w);
        axl_json_comment(&w, "one\nend */ after");
        axl_json_kv_int(&w, "k", 1);
        axl_json_obj_end(&w);
        axl_json_writer_finish(&w);
        test_check(axl_strcmp(axl_string_str(out),
                              "{/* one\nend * / after */\"k\":1}") == 0,
                   "comment ml: a close-comment sequence on a LATER line is "
                   "still split — the sanitizer did not stop at line one");
    }
}

// ---------------------------------------------------------------------------
// ENSURE_ASCII (P6) — the surrogate-pair boundary
//
// The design doc calls this the fiddliest piece of the redesign, because
// `\uXXXX` carries 16 bits and a non-BMP code point needs a PAIR. The
// boundary is asserted exactly rather than sampled.
// ---------------------------------------------------------------------------

static void
ascii_check(const char *utf8, const char *want, const char *msg)
{
    AXL_AUTOPTR(AxlString) out = axl_string_new(NULL);
    AxlJsonWriter          w;

    axl_json_writer_init(&w, out, AXL_JSON_ENSURE_ASCII);
    axl_json_obj_begin(&w);
    axl_json_kv_str(&w, "k", utf8);
    axl_json_obj_end(&w);
    axl_json_writer_finish(&w);
    test_check(!axl_json_writer_error(&w)
               && axl_strcmp(axl_string_str(out), want) == 0, msg);
}

/* Write one ill-formed byte under @a flags and compare the whole document. */
static void
wr_utf8_check(AxlJsonFlags flags, const char *value, const char *want,
              const char *msg)
{
    AXL_AUTOPTR(AxlString) out = axl_string_new(NULL);
    AxlJsonWriter          w;

    axl_json_writer_init(&w, out, flags);
    axl_json_obj_begin(&w);
    axl_json_kv_str(&w, "k", value);
    axl_json_obj_end(&w);
    axl_json_writer_finish(&w);
    test_check(!axl_json_writer_error(&w)
               && axl_strcmp(axl_string_str(out), want) == 0, msg);
}

/* Parse @a doc, expect FAILURE, and compare the whole rendered diagnostic. */
static void
errfmt_check(const char *doc, AxlJsonFlags flags, bool quote,
             const char *want, const char *msg)
{
    AxlJsonReader r;
    char          buf[256];
    int           n;

    if (axl_json_parse(doc, axl_strlen(doc), flags, &r)) {
        axl_json_free(&r);
        test_check(false, msg);
        return;
    }
    n = axl_json_error_format(axl_json_reader_error(&r),
                              quote ? doc : NULL,
                              quote ? axl_strlen(doc) : 0,
                              buf, sizeof(buf));
    test_check(n > 0 && axl_strcmp(buf, want) == 0
               && (size_t)n == axl_strlen(want), msg);
    axl_json_free(&r);
}

// ---------------------------------------------------------------------------
// Writer -> reader round trip: what we emit must PARSE, not merely match
// ---------------------------------------------------------------------------
//
// The formatting-flag tests assert exact bytes, which pins WHAT we emit and
// says nothing about whether it is valid JSON. Those are different
// properties, and every flag added in P5, P6 and P8 had only the first. A
// writer that emitted a stray comma, an unterminated escape or a broken
// surrogate pair would satisfy an exact-string test that was updated to match
// it — the assertion moves with the bug.
//
// One document, built identically under every flag set, read back and checked
// value by value. Two flags deliberately do NOT produce a parseable document
// and are pinned separately below.

/* The document every round-trip row builds. Deliberately carries a slash (for
   ESCAPE_SLASH), a 2-byte character (for ENSURE_ASCII), a negative number, a
   nested object and an array — so a flag that mangles any one of those is
   caught by the value checks rather than by the parse alone. */
static void
rt_build(AxlJsonWriter *w)
{
    axl_json_obj_begin(w);
    axl_json_kv_str(w, "path", "a/b");
    /* 2-byte AND 4-byte. The 4-byte one is what makes the ENSURE_ASCII
       row exercise a SURROGATE PAIR — with only `caf\xC3\xA9` a broken pair
       slips through this pass entirely, which a sabotage demonstrated. */
    axl_json_kv_str(w, "text", "caf\xC3\xA9 \xF0\x9F\x98\x80");
    /* A quote and a control character: the two things that MUST be escaped
       for the output to be valid at all. Without them a writer that stopped
       escaping would still produce a parseable document, and this whole pass
       would be checking nothing an exact-string test does not already. */
    axl_json_kv_str(w, "esc", "he said \"hi\"\nand left");
    axl_json_kv_int(w, "n", -42);
    axl_json_key(w, "arr");
    axl_json_arr_begin(w);
    axl_json_int(w, 1);
    axl_json_str(w, "x/y");
    axl_json_arr_end(w);
    axl_json_key(w, "obj");
    axl_json_obj_begin(w);
    axl_json_kv_bool(w, "flag", true);
    axl_json_obj_end(w);
    axl_json_obj_end(w);
}

/* Every value rt_build wrote, read back off @a r. */
static bool
rt_verify(const AxlJsonReader *r)
{
    AxlJsonReader    sub;
    AxlJsonArrayIter it;
    AxlJsonReader    elem;
    char             buf[48];
    int64_t          n    = 0;
    bool             flag = false;
    int64_t          e0   = 0;

    if (!axl_json_get_string(r, "path", buf, sizeof(buf))
        || axl_strcmp(buf, "a/b") != 0) {
        return false;       /* ESCAPE_SLASH must survive the round trip */
    }
    if (!axl_json_get_string(r, "text", buf, sizeof(buf))
        || axl_strcmp(buf, "caf\xC3\xA9 \xF0\x9F\x98\x80") != 0) {
        return false;       /* ENSURE_ASCII must be LOSSLESS, not lossy */
    }
    if (!axl_json_get_string(r, "esc", buf, sizeof(buf))
        || axl_strcmp(buf, "he said \"hi\"\nand left") != 0) {
        return false;       /* escaping is what keeps the document valid */
    }
    if (!axl_json_get_int(r, "n", &n) || n != -42) {
        return false;
    }
    if (!axl_json_get_object(r, "obj", &sub)
        || !axl_json_get_bool(&sub, "flag", &flag) || !flag) {
        return false;       /* nesting survived the indent changes */
    }
    if (!axl_json_array_begin(r, "arr", &it)
        || !axl_json_array_next(&it, &elem)
        || !axl_json_value_int(&elem, &e0) || e0 != 1) {
        return false;
    }
    if (!axl_json_array_next(&it, &elem)
        || !axl_json_value_string(&elem, buf, sizeof(buf))
        || axl_strcmp(buf, "x/y") != 0) {
        return false;
    }
    return true;
}

/* Build under @a wflags, parse back under @a rflags, check every value. */
static void
rt_check(AxlJsonFlags wflags, AxlJsonFlags rflags, const char *msg)
{
    AXL_AUTOPTR(AxlString) out = axl_string_new(NULL);
    AxlJsonWriter          w;
    AxlJsonReader          r;
    bool                   ok;

    axl_json_writer_init(&w, out, wflags);
    rt_build(&w);
    axl_json_writer_finish(&w);
    if (axl_json_writer_error(&w)) {
        test_check(false, msg);
        return;
    }
    ok = axl_json_parse(axl_string_str(out),
                              axl_strlen(axl_string_str(out)), rflags, &r)
         && rt_verify(&r);
    axl_json_free(&r);
    test_check(ok, msg);
}

// ---------------------------------------------------------------------------
// Encoding and line endings on the JSON boundary
// ---------------------------------------------------------------------------
//
// The JSON layer is UTF-8 only and says so: a UTF-16 or BOM-prefixed document
// is one of the two DELIBERATE narrowings axl_json_parse documents, on
// the grounds that RFC 8259 §8.1 requires UTF-8 for interchange and AXL does
// not sniff or transcode. UEFI, though, is exactly the "closed ecosystem" that
// sentence carves out, and UCS-2 is its native text form — the shell's pipes
// use it.
//
// Both are true at once because the transcoding lives one layer down, in
// AxlStream. These tests pin the composition, which nothing did: the only
// UCS-2 tests in the tree were compress filters REFUSING a transcoding peer.

/* Build a buffer stream holding @a n raw bytes, rewound and ready to read. */
static AxlStream *
enc_bytes_stream(const void *bytes, size_t n)
{
    AxlStream *s = axl_bufopen();

    if (s == NULL) {
        return NULL;
    }
    axl_fwrite(bytes, 1, n, s);
    axl_fflush(s);
    axl_fseek(s, 0, AXL_SEEK_SET);
    return s;
}

/* Parse whatever @a s yields and check `{"a":1,"b":"x"}` came through. */
static bool
enc_parse_and_verify(AxlStream *s, AxlJsonFlags flags)
{
    AxlJsonSource src;
    AxlJsonReader r;
    char          buf[16];
    int64_t       a  = 0;
    bool          ok;

    axl_json_source_init_stream(&src, s);
    ok = axl_json_parse_source(&src, flags, &r)
         && axl_json_get_int(&r, "a", &a) && a == 1
         && axl_json_get_string(&r, "b", buf, sizeof(buf))
         && axl_strcmp(buf, "x") == 0;
    axl_json_free(&r);
    return ok;
}

static void
test_json_encoding_boundary(void)
{
    /* `{"a":1,"b":"x"}` as UCS-2, both byte orders, and with a BOM. */
    static const unsigned char le[] = {
        0x7B,0x00, 0x22,0x00, 0x61,0x00, 0x22,0x00, 0x3A,0x00, 0x31,0x00,
        0x2C,0x00, 0x22,0x00, 0x62,0x00, 0x22,0x00, 0x3A,0x00, 0x22,0x00,
        0x78,0x00, 0x22,0x00, 0x7D,0x00
    };
    static const unsigned char be[] = {
        0x00,0x7B, 0x00,0x22, 0x00,0x61, 0x00,0x22, 0x00,0x3A, 0x00,0x31,
        0x00,0x2C, 0x00,0x22, 0x00,0x62, 0x00,0x22, 0x00,0x3A, 0x00,0x22,
        0x00,0x78, 0x00,0x22, 0x00,0x7D
    };

    // --- WRITING JSON onto a UCS-2 wire ------------------------------------
    {
        AxlStream          *s = axl_bufopen();
        AxlJsonSink         snk;
        AxlJsonWriter       w;
        const unsigned char *raw;
        size_t              n = 0;

        axl_stream_set_encoding(s, AXL_ENC_UCS2_LE);
        axl_json_sink_init_stream(&snk, s);
        axl_json_writer_init_sink(&w, &snk, AXL_JSON_STRICT);
        axl_json_obj_begin(&w);
        axl_json_kv_int(&w, "a", 1);
        axl_json_kv_str(&w, "b", "x");
        axl_json_obj_end(&w);
        axl_json_writer_finish(&w);
        axl_fflush(s);

        raw = axl_bufdata(s, &n);
        test_check(!axl_json_writer_error(&w) && raw != NULL
                   && n == sizeof(le)
                   && axl_memcmp(raw, le, sizeof(le)) == 0,
                   "encoding: the writer's UTF-8 output lands as UCS-2 LE on "
                   "the wire — the transcode is the stream's, not JSON's");
        axl_fclose(s);
    }

    // --- READING a UCS-2 document, both byte orders ------------------------
    {
        AxlStream *s = enc_bytes_stream(le, sizeof(le));

        axl_stream_set_encoding(s, AXL_ENC_UCS2_LE);
        test_check(enc_parse_and_verify(s, AXL_JSON_STRICT),
                   "encoding: a UCS-2 LE document parses through a stream "
                   "source, values intact");
        axl_fclose(s);
    }
    {
        AxlStream *s = enc_bytes_stream(be, sizeof(be));

        axl_stream_set_encoding(s, AXL_ENC_UCS2_BE);
        test_check(enc_parse_and_verify(s, AXL_JSON_STRICT),
                   "encoding: and UCS-2 BE too — the rarer order is not the "
                   "untested one");
        axl_fclose(s);
    }

    // --- the BOM: refused by JSON, consumed by the text wrapper ------------
    // Both halves of the documented position, so neither can drift. A UEFI
    // tool writing a UCS-2 file almost always emits FF FE first, and without
    // the wrapper that arrives as a stray U+FEFF ahead of the `{`.
    {
        unsigned char bom_le[2 + sizeof(le)];
        AxlStream    *raw_s;
        AxlStream    *txt;

        bom_le[0] = 0xFF;
        bom_le[1] = 0xFE;
        axl_memcpy(bom_le + 2, le, sizeof(le));

        /* Declaring the encoding by hand does NOT skip the BOM: it decodes to
           U+FEFF, which is not whitespace, so the parse fails. */
        raw_s = enc_bytes_stream(bom_le, sizeof(bom_le));
        axl_stream_set_encoding(raw_s, AXL_ENC_UCS2_LE);
        test_check(!enc_parse_and_verify(raw_s, AXL_JSON_STRICT),
                   "encoding: a BOM is NOT skipped by set_encoding alone — it "
                   "decodes to U+FEFF and the document is refused");
        axl_fclose(raw_s);

        /* The text wrapper is what consumes it, and it detects the order too,
           so the caller does not have to know. */
        raw_s = enc_bytes_stream(bom_le, sizeof(bom_le));
        txt   = axl_text_stream_wrap(raw_s);
        test_check(txt != NULL && enc_parse_and_verify(txt, AXL_JSON_STRICT),
                   "encoding: axl_text_stream_wrap consumes the BOM and picks "
                   "the byte order, and then the document parses");
        axl_fclose(txt);
        axl_fclose(raw_s);
    }
    {
        /* Same for a UTF-8 BOM, which is the shape a Windows-authored config
           arrives in. JSON refuses it directly; the wrapper eats it. */
        static const unsigned char u8bom[] = {
            0xEF,0xBB,0xBF, '{','"','a','"',':','1',',',
            '"','b','"',':','"','x','"','}'
        };
        AxlJsonReader r;
        AxlStream    *raw_s;
        AxlStream    *txt;

        test_check(!axl_json_parse((const char *)u8bom, sizeof(u8bom), AXL_JSON_RELAXED, &r),
                   "encoding: a UTF-8 BOM in a contiguous buffer is REFUSED — "
                   "AXL does not sniff, as the contract says");
        axl_json_free(&r);

        raw_s = enc_bytes_stream(u8bom, sizeof(u8bom));
        txt   = axl_text_stream_wrap(raw_s);
        test_check(txt != NULL && enc_parse_and_verify(txt, AXL_JSON_STRICT),
                   "encoding: and the wrapper consumes it, leaving a document "
                   "that parses");
        axl_fclose(txt);
        axl_fclose(raw_s);
    }
}

static void
test_json_line_endings(void)
{
    // --- CRLF and LF are both accepted, and mean the same thing ------------
    // RFC 8259 §2 lists space, tab, LF and CR as whitespace, so a CRLF
    // document is ordinary JSON rather than a tolerated deviation. Asserted
    // as an EQUIVALENCE: the same document under both endings must read back
    // identically, which a test of CRLF alone would not show.
    {
        const char *lf   = "{\n  \"a\": 1,\n  \"b\": \"x\"\n}";
        const char *crlf = "{\r\n  \"a\": 1,\r\n  \"b\": \"x\"\r\n}";
        AxlJsonReader r;
        char          buf[16];
        int64_t       a  = 0;
        bool          lf_ok;
        bool          crlf_ok;

        lf_ok = axl_json_parse(lf, axl_strlen(lf), AXL_JSON_RELAXED, &r)
                && axl_json_get_int(&r, "a", &a) && a == 1
                && axl_json_get_string(&r, "b", buf, sizeof(buf))
                && axl_strcmp(buf, "x") == 0;
        axl_json_free(&r);

        a = 0;
        buf[0] = '\0';
        crlf_ok = axl_json_parse(crlf, axl_strlen(crlf), AXL_JSON_RELAXED, &r)
                  && axl_json_get_int(&r, "a", &a) && a == 1
                  && axl_json_get_string(&r, "b", buf, sizeof(buf))
                  && axl_strcmp(buf, "x") == 0;
        axl_json_free(&r);

        test_check(lf_ok && crlf_ok,
                   "line endings: LF and CRLF documents both parse to the "
                   "same values — CR is whitespace, not a tolerated quirk");
    }

    // --- a lone CR is whitespace too ---------------------------------------
    // Classic Mac line endings, and the case a "\\r\\n only" implementation
    // would get wrong.
    {
        const char   *cr = "{\r  \"a\": 1\r}";
        AxlJsonReader r;
        int64_t       a  = 0;

        test_check(axl_json_parse(cr, axl_strlen(cr), AXL_JSON_RELAXED, &r)
                   && axl_json_get_int(&r, "a", &a) && a == 1,
                   "line endings: a lone CR is whitespace as well — RFC 8259 "
                   "lists it, so CR-only documents are not a special case");
        axl_json_free(&r);
    }

    // --- the error position counts LINES, not CR bytes ---------------------
    // A CRLF document must not report line 5 for what a human sees as line 3.
    {
        const char         *doc = "{\r\n  \"a\": 1,\r\n  \"b\" 2\r\n}";
        AxlJsonReader       r;
        const AxlJsonError *e;

        test_check(!axl_json_parse(doc, axl_strlen(doc), AXL_JSON_RELAXED, &r),
                   "line endings: the malformed CRLF document is refused");
        e = axl_json_reader_error(&r);
        test_check(e->line == 3,
                   "line endings: and it is reported on line 3 — the CR does "
                   "not count as a line of its own");
        axl_json_free(&r);
    }

    // --- a JSON5 line comment ends at a LONE CR ----------------------------
    // CRLF proves nothing here: the comment would end at the LF anyway, with
    // the CR harmlessly inside its body. Only a CR-only ending makes the
    // terminator load-bearing — without it the comment swallows the rest of
    // the document. A sabotage of the CR branch passed the CRLF version.
    {
        const char   *doc = "{// note\r\"a\":1}";
        AxlJsonReader r;
        int64_t       a   = 0;

        test_check(axl_json_parse(doc, axl_strlen(doc),
                                        AXL_JSON_ALLOW_COMMENTS, &r)
                   && axl_json_get_int(&r, "a", &a) && a == 1,
                   "line endings: a JSON5 line comment terminates at a LONE "
                   "CR, so the member after it is not swallowed");
        axl_json_free(&r);
    }
}

static void
test_json_writer_roundtrip(void)
{
    // --- every formatting flag produces a document that PARSES -------------
    rt_check(AXL_JSON_STRICT, AXL_JSON_STRICT,
             "roundtrip: compact output parses back with every value intact");
    rt_check(AXL_JSON_INDENT(0), AXL_JSON_STRICT,
             "roundtrip: INDENT(0) — newlines at zero indent");
    rt_check(AXL_JSON_INDENT(2), AXL_JSON_STRICT,
             "roundtrip: INDENT(2)");
    rt_check(AXL_JSON_INDENT(8), AXL_JSON_STRICT,
             "roundtrip: INDENT(8) — a wide indent is still whitespace");
    rt_check(AXL_JSON_COMPACT, AXL_JSON_STRICT,
             "roundtrip: COMPACT alone");
    rt_check(AXL_JSON_INDENT(2) | AXL_JSON_COMPACT, AXL_JSON_STRICT,
             "roundtrip: INDENT(2) | COMPACT — newlines without the space "
             "after the colon");
    rt_check(AXL_JSON_ESCAPE_SLASH, AXL_JSON_STRICT,
             "roundtrip: ESCAPE_SLASH — `\\/` is a valid escape and decodes "
             "back to a plain slash");
    rt_check(AXL_JSON_ENSURE_ASCII, AXL_JSON_STRICT,
             "roundtrip: ENSURE_ASCII — the escaped form decodes back to the "
             "SAME bytes, so the escaping is lossless");
    rt_check(AXL_JSON_INDENT(2) | AXL_JSON_ESCAPE_SLASH
             | AXL_JSON_ENSURE_ASCII, AXL_JSON_STRICT,
             "roundtrip: all three per-value flags at once");

    // --- the JSON5 writer output needs a JSON5 reader -----------------------
    // A trailing comma is the one thing the writer emits that strict JSON
    // refuses, so this row is also the proof that the dialect bit is doing
    // something on BOTH sides.
    rt_check(AXL_JSON_ALLOW_TRAILING_COMMA,
             AXL_JSON_ALLOW_TRAILING_COMMA,
             "roundtrip: a trailing-comma document parses back under "
             "ALLOW_TRAILING_COMMA");
    {
        AXL_AUTOPTR(AxlString) out = axl_string_new(NULL);
        AxlJsonWriter          w;
        AxlJsonReader          r;

        axl_json_writer_init(&w, out, AXL_JSON_ALLOW_TRAILING_COMMA);
        rt_build(&w);
        axl_json_writer_finish(&w);
        test_check(!axl_json_parse(axl_string_str(out),
                                         axl_strlen(axl_string_str(out)),
                                         AXL_JSON_STRICT, &r),
                   "roundtrip: and STRICT REFUSES it — the round trip is "
                   "dialect-matched, not accidental");
        axl_json_free(&r);
    }

    // --- SORT_KEYS goes through write_token, so it round-trips there -------
    {
        const char            *doc = "{\"c\":1,\"a\":{\"z\":2,\"y\":3}}";
        AXL_AUTOPTR(AxlString) out = axl_string_new(NULL);
        AxlJsonWriter          w;
        AxlJsonReader          r;
        AxlJsonReader          back;
        AxlJsonReader          sub;
        int64_t                c = 0;
        int64_t                y = 0;

        test_check(axl_json_parse(doc, axl_strlen(doc), AXL_JSON_RELAXED, &r),
                   "roundtrip: the sort source parses");
        axl_json_writer_init(&w, out, AXL_JSON_SORT_KEYS);
        axl_json_write_token(&w, &r, 0);
        axl_json_writer_finish(&w);
        axl_json_free(&r);

        /* Reordering members must not change what the document SAYS. */
        test_check(axl_json_parse(axl_string_str(out),
                                  axl_strlen(axl_string_str(out)), AXL_JSON_RELAXED, &back)
                   && axl_json_get_int(&back, "c", &c) && c == 1
                   && axl_json_get_object(&back, "a", &sub)
                   && axl_json_get_int(&sub, "y", &y) && y == 3,
                   "roundtrip: SORT_KEYS output parses and every key still "
                   "reads back, nested ones included");
        axl_json_free(&back);
    }

    // --- EMBED does NOT produce a document, and that is the contract -------
    // Wrapping its output in the delimiter it omitted must reproduce a
    // parseable document. That is the identity EMBED is defined by, checked
    // here as validity rather than as bytes.
    {
        AXL_AUTOPTR(AxlString) emb  = axl_string_new(NULL);
        AXL_AUTOPTR(AxlString) whole = axl_string_new(NULL);
        AxlJsonWriter          w;
        AxlJsonReader          r;

        axl_json_writer_init(&w, emb, AXL_JSON_EMBED);
        rt_build(&w);
        axl_json_writer_finish(&w);

        test_check(!axl_json_parse(axl_string_str(emb),
                                   axl_strlen(axl_string_str(emb)), AXL_JSON_RELAXED, &r),
                   "roundtrip: EMBED output is NOT a document on its own — "
                   "the outer braces are gone by design");
        axl_json_free(&r);

        axl_string_append(whole, "{");
        axl_string_append(whole, axl_string_str(emb));
        axl_string_append(whole, "}");
        test_check(axl_json_parse(axl_string_str(whole),
                                  axl_strlen(axl_string_str(whole)), AXL_JSON_RELAXED, &r)
                   && rt_verify(&r),
                   "roundtrip: but wrapped in the delimiter it omitted it "
                   "parses, with every value intact");
        axl_json_free(&r);
    }

    // --- UTF8_RAW deliberately emits text that is not well-formed JSON -----
    // RFC 8259 defines a JSON text over Unicode code points, so a raw
    // ill-formed byte makes the document invalid BY THE SPEC. Our own reader
    // accepts it anyway unless UTF8_STRICT is asked for — that asymmetry is
    // real, deliberate, and pinned here because nothing else states it.
    {
        AXL_AUTOPTR(AxlString) out = axl_string_new(NULL);
        AxlJsonWriter          w;
        AxlJsonReader          r;
        char                   buf[16];

        axl_json_writer_init(&w, out, AXL_JSON_UTF8_RAW);
        axl_json_obj_begin(&w);
        axl_json_kv_str(&w, "k", "a\x80z");
        axl_json_obj_end(&w);
        axl_json_writer_finish(&w);

        test_check(!axl_json_writer_error(&w)
                   && axl_json_parse(axl_string_str(out),
                                           axl_strlen(axl_string_str(out)),
                                           AXL_JSON_UTF8_RAW, &r)
                   && axl_json_get_string(&r, "k", buf, sizeof(buf))
                   && axl_strcmp(buf, "a\x80z") == 0,
                   "roundtrip: UTF8_RAW round-trips its bytes exactly — read "
                   "and write agree, which is the whole point of the mode");
        axl_json_free(&r);

        test_check(!axl_json_parse(axl_string_str(out),
                                         axl_strlen(axl_string_str(out)),
                                         AXL_JSON_UTF8_STRICT, &r),
                   "roundtrip: and UTF8_STRICT refuses it, because that "
                   "output is not well-formed JSON text by RFC 8259");
        axl_json_free(&r);
    }

    // --- REPAIR is the mode that keeps the output valid --------------------
    {
        AXL_AUTOPTR(AxlString) out = axl_string_new(NULL);
        AxlJsonWriter          w;
        AxlJsonReader          r;
        char                   buf[16];

        axl_json_writer_init(&w, out, AXL_JSON_STRICT);   /* REPAIR is zero */
        axl_json_obj_begin(&w);
        axl_json_kv_str(&w, "k", "a\x80z");
        axl_json_obj_end(&w);
        axl_json_writer_finish(&w);

        test_check(axl_json_parse(axl_string_str(out),
                                        axl_strlen(axl_string_str(out)),
                                        AXL_JSON_UTF8_STRICT, &r)
                   && axl_json_get_string(&r, "k", buf, sizeof(buf))
                   && axl_strcmp(buf, "a\xEF\xBF\xBDz") == 0,
                   "roundtrip: REPAIR output survives even a UTF8_STRICT "
                   "parse — repairing is what keeps the document valid");
        axl_json_free(&r);
    }
}

static void
test_json_get_double(void)
{
    // --- the ordinary shapes, exact ----------------------------------------
    // Bit-exact comparisons: a float test that allows an epsilon cannot tell
    // correct rounding from nearly-correct, which is the whole property the
    // underlying parser promises.
    {
        struct { const char *doc; double want; const char *msg; } row[] = {
            { "{\"v\":1.5}",    1.5,    "a plain fraction" },
            { "{\"v\":-2.25}", -2.25,   "a negative fraction" },
            { "{\"v\":0}",      0.0,    "an integral literal, as a double" },
            { "{\"v\":1e3}",    1000.0, "scientific notation" },
            { "{\"v\":1E-2}",   0.01,   "a negative exponent" },
            { "{\"v\":0.1}",    0.1,    "the NEAREST double, not drifted" },
            { "{\"v\":123456789.125}", 123456789.125,
              "more precision than a float holds" },
        };
        size_t i;
        bool   all_exact = true;

        for (i = 0; i < sizeof(row) / sizeof(row[0]); i++) {
            AxlJsonReader r;
            double        got = 42.0;

            if (!axl_json_parse(row[i].doc, axl_strlen(row[i].doc), AXL_JSON_RELAXED, &r)) {
                all_exact = false;
                axl_json_free(&r);
                continue;
            }
            if (!axl_json_get_double(&r, "v", &got) || got != row[i].want) {
                all_exact = false;
            }
            axl_json_free(&r);
        }
        test_check(all_exact,
                   "get_double: every ordinary shape reads back bit-exact — "
                   "integral, fractional, signed and scientific");
    }

    // --- the WHOLE token must parse ----------------------------------------
    // This is what separates it from get_int, which truncates at the first
    // non-digit so `1.5` yields 1. There is no sensible prefix of a float, and
    // JSON5's hex literal is the case that makes it concrete: `0x1F` would
    // otherwise parse as 0 and stop at the `x`, handing back a number the
    // document does not contain.
    {
        AxlJsonReader r;
        double        got = 42.0;
        int64_t       n   = 0;

        test_check(axl_json_parse("{\"v\":0x1F}", 10,
                                        AXL_JSON_JSON5, &r),
                   "get_double: the JSON5 hex document parses");
        test_check(!axl_json_get_double(&r, "v", &got) && got == 42.0,
                   "get_double: a hex literal is REFUSED, not read as 0 — the "
                   "token must parse entirely");
        test_check(axl_json_get_int(&r, "v", &n) && n == 31,
                   "get_double: and get_int is where hex still works, so the "
                   "refusal costs the caller nothing");
        axl_json_free(&r);
    }

    // --- NaN and Infinity round-trip when the dialect allowed them ---------
    // The lexer is the dialect gate; by the time a token reaches the accessor
    // it is already permitted. Both spellings are checked because the decimal
    // parser matches them case-insensitively and tries "infinity" before
    // "inf" — if it stopped at "inf" the whole-token rule would reject the
    // JSON5 spelling outright.
    {
        struct { const char *doc; const char *msg; bool want_inf;
                 bool want_neg; } row[] = {
            { "{\"v\":Infinity}",
              "get_double: JSON5 Infinity reads back as an infinity", true,
              false },
            { "{\"v\":-Infinity}",
              "get_double: and -Infinity keeps its sign", true, true },
        };
        size_t i;
        AxlJsonReader r;
        double        got;

        for (i = 0; i < sizeof(row) / sizeof(row[0]); i++) {
            got = 0.0;
            if (!axl_json_parse(row[i].doc, axl_strlen(row[i].doc),
                                      AXL_JSON_JSON5, &r)) {
                test_check(false, row[i].msg);
                axl_json_free(&r);
                continue;
            }
            test_check(axl_json_get_double(&r, "v", &got)
                       && axl_isinf(got)
                       && ((got < 0.0) == row[i].want_neg),
                       row[i].msg);
            axl_json_free(&r);
        }

        got = 0.0;
        test_check(axl_json_parse("{\"v\":NaN}", 9, AXL_JSON_JSON5, &r)
                   && axl_json_get_double(&r, "v", &got) && axl_isnan(got),
                   "get_double: JSON5 NaN reads back as a NaN — the parser "
                   "matches the spelling case-insensitively");
        axl_json_free(&r);
    }

    // --- out of range is a FAILURE, not an infinity ------------------------
    // axl_str_to_double reports overflow as the correct IEEE result together
    // with its error. This accessor does not pass that on: a `true` return
    // has to keep meaning "you got the number that is in the document".
    {
        struct { const char *doc; const char *msg; } row[] = {
            { "{\"v\":1e400}",      "1e400 (overflow)" },
            { "{\"v\":-1e400}",     "-1e400 (overflow, negative)" },
            { "{\"v\":1e-400}",     "1e-400 (UNDERFLOW — a different "
                                     "branch of the parser from overflow)" },
            { "{\"v\":-1e-400}",    "-1e-400 (underflow, negative)" },
        };
        size_t i;
        bool   all_refused = true;
        char   why[128];

        axl_snprintf(why, sizeof(why),
                     "get_double: an out-of-range magnitude is refused with "
                     "the caller's value untouched, over and under");

        for (i = 0; i < sizeof(row) / sizeof(row[0]); i++) {
            AxlJsonReader r;
            double        got = 42.0;

            if (!axl_json_parse(row[i].doc, axl_strlen(row[i].doc), AXL_JSON_RELAXED, &r)) {
                all_refused = false;
                axl_json_free(&r);
                continue;
            }
            if (axl_json_get_double(&r, "v", &got) || got != 42.0) {
                all_refused = false;   /* refused AND left untouched */
                axl_snprintf(why, sizeof(why),
                             "get_double: NOT refused: %s", row[i].msg);
            }
            axl_json_free(&r);
        }
        test_check(all_refused, why);
    }

    // --- a LONG literal is not a shorter one -------------------------------
    // The first version copied the token into a fixed 64-byte buffer and
    // refused anything longer, on the reasoning that "the longest meaningful
    // form is well under 40 characters". Legal JSON has no length limit on a
    // number, and axl-strtod.c sizes its own accumulator for a ~768-digit
    // significand — it was built for exactly these. `1.` and seventy zeros is
    // 1.0, and refusing it reported a valid document as "not a number".
    //
    // Swept across the old boundary rather than probed at one length, because
    // a single case lands wherever it lands: this is the sweep that found the
    // defect.
    {
        size_t pad;
        bool   all_read = true;
        char   why[160];

        axl_snprintf(why, sizeof(why),
                     "get_double: a literal of any length reads back — the "
                     "value is the number, not the byte count");

        for (pad = 55; pad <= 80; pad++) {
            AXL_AUTOPTR(AxlString) doc = axl_string_new(NULL);
            AxlJsonReader          r;
            double                 got = 42.0;
            size_t                 i;

            /* `1.` + `pad` zeros: exactly 1.0 however long it is written. */
            axl_string_append(doc, "{\"v\":1.");
            for (i = 0; i < pad; i++) {
                axl_string_append(doc, "0");
            }
            axl_string_append(doc, "}");

            if (!axl_json_parse(axl_string_str(doc),
                                axl_strlen(axl_string_str(doc)), AXL_JSON_RELAXED, &r)
                || !axl_json_get_double(&r, "v", &got) || got != 1.0) {
                all_read = false;
                axl_snprintf(why, sizeof(why),
                             "get_double: a %zu-digit fraction was refused or "
                             "misread", pad);
            }
            axl_json_free(&r);
        }
        test_check(all_read, why);
    }

    // --- negative zero keeps its sign --------------------------------------
    // `got != want` cannot see this: -0.0 == 0.0 is true, so the sign has to
    // be tested directly or a lost sign passes every other row here.
    {
        AxlJsonReader r;
        double        got = 1.0;
        uint64_t      bits = 0;

        test_check(axl_json_parse("{\"v\":-0.0}", 10, AXL_JSON_RELAXED, &r)
                   && axl_json_get_double(&r, "v", &got) && got == 0.0,
                   "get_double: -0.0 reads back as a zero");
        /* The sign lives in the top bit; there is no axl_signbit, and an
           equality test cannot see it because -0.0 == 0.0. */
        axl_memcpy(&bits, &got, sizeof(bits));
        test_check((bits >> 63) == 1u,
                   "get_double: and it KEEPS its sign — an equality check "
                   "alone cannot tell -0.0 from +0.0");
        axl_json_free(&r);
    }

    // --- the JSON5 number shapes the dialect permits -----------------------
    // Hex is the one the docstring names, so these are the ones that could
    // regress silently under the whole-token rule.
    {
        struct { const char *doc; double want; } row[] = {
            { "{\"v\":+5}",   5.0  },
            { "{\"v\":.5}",   0.5  },
            { "{\"v\":5.}",   5.0  },
        };
        size_t i;
        bool   all_read = true;

        for (i = 0; i < sizeof(row) / sizeof(row[0]); i++) {
            AxlJsonReader r;
            double        got = 42.0;

            if (!axl_json_parse(row[i].doc, axl_strlen(row[i].doc),
                                      AXL_JSON_JSON5, &r)
                || !axl_json_get_double(&r, "v", &got)
                || got != row[i].want) {
                all_read = false;
            }
            axl_json_free(&r);
        }
        test_check(all_read,
                   "get_double: JSON5 +5, .5 and 5. all parse WHOLE — the "
                   "rule that refuses hex must not refuse these");
    }

    // --- value_double on a CONTAINER, directly -----------------------------
    // The type guard is otherwise only reached through get_double on a
    // string, so nothing exercises it for an object or an array.
    {
        AxlJsonReader r;
        AxlJsonReader sub;
        double        got = 42.0;

        test_check(axl_json_parse("{\"o\":{\"a\":1},\"n\":1.5}", 21, AXL_JSON_RELAXED, &r),
                   "get_double: the container document parses");
        test_check(axl_json_get_value(&r, "o", &sub)
                   && !axl_json_value_double(&sub, &got) && got == 42.0,
                   "value_double: an OBJECT is refused with the value "
                   "untouched, like every non-primitive");
        axl_json_free(&r);
    }

    // --- what is not a number ----------------------------------------------
    {
        AxlJsonReader r;
        double        got = 42.0;
        const char   *doc = "{\"s\":\"1.5\",\"b\":true,\"n\":null}";

        test_check(axl_json_parse(doc, axl_strlen(doc), AXL_JSON_RELAXED, &r),
                   "get_double: the non-number document parses");
        test_check(!axl_json_get_double(&r, "s", &got)
                   && !axl_json_get_double(&r, "b", &got)
                   && !axl_json_get_double(&r, "n", &got)
                   && !axl_json_get_double(&r, "absent", &got)
                   && got == 42.0,
                   "get_double: a string, a bool, a null and an absent key "
                   "are all refused with the value untouched");
        axl_json_free(&r);
    }

    // --- the own-value mirror agrees with the by-key form ------------------
    // P11's rule: get_X is get_value + value_X, so the two cannot drift.
    {
        AxlJsonReader r;
        AxlJsonReader v;
        AxlJsonArrayIter it;
        AxlJsonReader elem;
        double        by_key = 0.0;
        double        by_val = 0.0;

        test_check(axl_json_parse("{\"v\":2.5,\"a\":[0.5,1e400]}", 25, AXL_JSON_RELAXED, &r),
                   "get_double: the mirror document parses");
        test_check(axl_json_get_double(&r, "v", &by_key)
                   && axl_json_get_value(&r, "v", &v)
                   && axl_json_value_double(&v, &by_val)
                   && by_key == by_val && by_key == 2.5,
                   "value_double: the own-value form agrees with the by-key "
                   "form exactly");

        /* And an ARRAY element, which has no key to look up at all. */
        by_val = 42.0;
        test_check(axl_json_array_begin(&r, "a", &it)
                   && axl_json_array_next(&it, &elem)
                   && axl_json_value_double(&elem, &by_val)
                   && by_val == 0.5,
                   "value_double: an array element reads as a double");
        /* The second element overflows: refused there too, same rule. */
        by_val = 42.0;
        test_check(axl_json_array_next(&it, &elem)
                   && !axl_json_value_double(&elem, &by_val)
                   && by_val == 42.0,
                   "value_double: and the mirror refuses an out-of-range "
                   "element on the same terms");
        axl_json_free(&r);
    }

    // --- argument handling --------------------------------------------------
    {
        AxlJsonReader r;
        double        got = 42.0;

        test_check(axl_json_parse("{\"v\":1.5}", 9, AXL_JSON_RELAXED, &r),
                   "get_double: the argument document parses");
        test_check(!axl_json_get_double(NULL, "v", &got)
                   && !axl_json_get_double(&r, NULL, &got)
                   && !axl_json_get_double(&r, "v", NULL)
                   && !axl_json_value_double(NULL, &got)
                   && got == 42.0,
                   "get_double: NULL arguments are refused, value untouched");
        axl_json_free(&r);
    }
}

static void
test_json_error_format(void)
{
    // --- the terse form, which is all a machine needs ----------------------
    errfmt_check("{\"a\" 1}", AXL_JSON_STRICT, false,
                 "1:6: unexpected byte",
                 "errfmt: terse form is LINE:COL: message");

    // --- and the quoted form, with the caret under the column --------------
    errfmt_check("{\"a\" 1}", AXL_JSON_STRICT, true,
                 "1:6: unexpected byte\n"
                 "{\"a\" 1}\n"
                 "     ^",
                 "errfmt: quoting the line puts the caret under the column");

    // --- the offending LINE, not the first one -----------------------------
    errfmt_check("{\n  \"a\": 1,\n  \"b\" 2\n}", AXL_JSON_STRICT, true,
                 "3:7: unexpected byte\n"
                 "  \"b\" 2\n"
                 "      ^",
                 "errfmt: a multi-line document quotes the OFFENDING line and "
                 "counts the column within it");

    // --- a TAB in the line is carried into the caret line ------------------
    errfmt_check("{\n\t\"b\" 2\n}", AXL_JSON_STRICT, true,
                 "2:6: unexpected byte\n"
                 "\t\"b\" 2\n"
                 "\t    ^",
                 "errfmt: a TAB is copied into the caret line as a TAB, so "
                 "the caret survives tab expansion");

    // --- a multi-byte character is ONE column ------------------------------
    errfmt_check("{\"\xC3\xA9\xC3\xA9\": 1 2}", AXL_JSON_STRICT, true,
                 "1:10: unexpected byte\n"
                 "{\"\xC3\xA9\xC3\xA9\": 1 2}\n"
                 "         ^",
                 "errfmt: a 2-byte character counts as ONE column, so the "
                 "caret does not drift right");

    // --- a TAB *after* a multi-byte character ------------------------------
    // The case that makes the caret walk characters rather than bytes. The
    // count of cells is the column either way, so only a tab whose position
    // the byte walk has not yet reached exposes the difference: walking bytes
    // spends two steps inside the 2-byte character and puts the second TAB one
    // cell late. A sabotage proved no other row here catches it.
    errfmt_check("{\n\t\"\xC3\xA9\":\t1 2\n}", AXL_JSON_STRICT, true,
                 "2:9: unexpected byte\n"
                 "\t\"\xC3\xA9\":\t1 2\n"
                 "\t    \t  ^",
                 "errfmt: a TAB that FOLLOWS a multi-byte character still "
                 "lands in the right cell — the caret walks characters");

    // --- CRLF: the trailing CR is not quoted -------------------------------
    // It would otherwise end the quote and shift the terminal.
    errfmt_check("{\r\n\"b\" 2\r\n}", AXL_JSON_STRICT, true,
                 "2:5: unexpected byte\n"
                 "\"b\" 2\n"
                 "    ^",
                 "errfmt: a CRLF line is quoted without its CR");

    // --- OK is not an error and says so ------------------------------------
    {
        AxlJsonReader r;
        char          buf[64];

        test_check(axl_json_parse("{\"a\":1}", 7, AXL_JSON_RELAXED, &r),
                   "errfmt: the successful parse succeeds");
        test_check(axl_json_error_format(axl_json_reader_error(&r),
                                         "{\"a\":1}", 7, buf, sizeof(buf)) > 0
                   && axl_strcmp(buf, "no error") == 0,
                   "errfmt: an OK record renders as `no error` alone — no "
                   "0:0 position, and no quote even with a document");
        axl_json_free(&r);
    }

    // --- DIALECT names the flag that would have accepted the input ---------
    // The one recoverable code in the enum, and the reason the record carries
    // a fifth field. Reporting the code without the flag delivers the half a
    // caller cannot act on.
    errfmt_check("{\"a\":1} // note", AXL_JSON_STRICT, false,
                 "1:9: feature needs a dialect flag "
                 "(pass AXL_JSON_ALLOW_COMMENTS)",
                 "errfmt: DIALECT names the flag to re-parse with");

    // --- the quote is SANITISED, because the document is untrusted ---------
    // This text goes to a console, and the library parses JSON off the
    // network. A raw ESC would carry an ANSI sequence out of a JSON body; a
    // raw CR would return the cursor to column 0 and wreck the quote and the
    // caret together; an embedded NUL would end the buffer early, making the
    // returned length longer than the caller can read.
    {
        struct { const char *doc; size_t len; const char *want;
                 const char *msg; } row[] = {
            { "{\"a\":\"x\x1By\"}", 11,
              "1:8: unexpected byte\n{\"a\":\"x?y\"}\n       ^",
              "errfmt: an ESC in the document becomes ? in the quote" },
            { "{\"a\":\"x\ry\"}", 11,
              "1:8: unexpected byte\n{\"a\":\"x?y\"}\n       ^",
              "errfmt: a mid-line CR becomes ? — it would otherwise return "
              "the cursor and wreck the caret" },
            { "{\"a\":\"x\0y\"}", 11,
              "1:8: unexpected byte\n{\"a\":\"x?y\"}\n       ^",
              "errfmt: an embedded NUL becomes ? — otherwise buf would end "
              "early and the length would outrun it" },
        };
        size_t i;

        for (i = 0; i < sizeof(row) / sizeof(row[0]); i++) {
            AxlJsonReader r;
            char          buf[160];
            int           n;

            if (axl_json_parse(row[i].doc, row[i].len,
                                     AXL_JSON_STRICT, &r)) {
                axl_json_free(&r);
                test_check(false, row[i].msg);
                continue;
            }
            n = axl_json_error_format(axl_json_reader_error(&r), row[i].doc,
                                      row[i].len, buf, sizeof(buf));
            test_check(n > 0 && axl_strcmp(buf, row[i].want) == 0
                       && (size_t)n == axl_strlen(buf), row[i].msg);
            axl_json_free(&r);
        }
    }

    // --- a long line is WINDOWED, not refused ------------------------------
    // Minified JSON is one line and is what machines emit, so refusing to
    // quote it would make this useless on exactly those documents. Asserted
    // as an exact document: the window's markers, its width and the caret
    // offset inside it are all arithmetic no substring probe would reach.
    {
        AXL_AUTOPTR(AxlString) doc = axl_string_new(NULL);
        AxlJsonReader          r;
        char                   buf[AXL_JSON_ERROR_BUF_MAX];
        size_t                 i;
        const char            *nl1;
        const char            *nl2;

        axl_string_append(doc, "{");
        for (i = 0; i < 20; i++) {
            char pair[24];

            axl_snprintf(pair, sizeof(pair), "\"a%02zu\":%zu,", i, i);
            axl_string_append(doc, pair);
        }
        axl_string_append(doc, "\"bad\" 1,");
        for (i = 0; i < 20; i++) {
            char pair[24];

            axl_snprintf(pair, sizeof(pair), "\"z%02zu\":%zu,", i, i);
            axl_string_append(doc, pair);
        }
        axl_string_append(doc, "\"end\":0}");

        test_check(!axl_json_parse(axl_string_str(doc),
                                         axl_strlen(axl_string_str(doc)),
                                         AXL_JSON_STRICT, &r),
                   "errfmt: the long document fails to parse");
        test_check(axl_json_error_format(axl_json_reader_error(&r),
                                         axl_string_str(doc),
                                         axl_strlen(axl_string_str(doc)),
                                         buf, sizeof(buf)) > 0,
                   "errfmt: the long document renders");

        nl1 = axl_strchr(buf, '\n');
        nl2 = (nl1 != NULL) ? axl_strchr(nl1 + 1, '\n') : NULL;
        test_check(nl1 != NULL && nl2 != NULL
                   && axl_strncmp(nl1 + 1, "...", 3) == 0
                   && axl_strncmp(nl2 - 3, "...", 3) == 0,
                   "errfmt: a long line is windowed, with ... marking BOTH "
                   "cut ends");
        /* The quote is the window plus its two markers, and the caret line
           opens with three spaces standing in for the leading marker so the
           caret still lines up under the source, not under the `...`. */
        test_check(nl2 != NULL
                   && (size_t)(nl2 - nl1 - 1) == AXL_JSON_ERROR_QUOTE_MAX + 6
                   && axl_strncmp(nl2 + 1, "   ", 3) == 0,
                   "errfmt: the window is exactly QUOTE_MAX wide plus its "
                   "markers, and the caret line pads under them");
        {
            /* And the caret sits where the window puts it: the column is
               centred, so it lands QUOTE_MAX/2 characters into the window,
               after the three spaces for the marker. */
            const char *caret = axl_strchr(nl2 + 1, '^');

            test_check(caret != NULL
                       && (size_t)(caret - (nl2 + 1))
                          == 3 + AXL_JSON_ERROR_QUOTE_MAX / 2,
                       "errfmt: and the caret is offset by col - skipped, not "
                       "by the raw column");
        }
        axl_json_free(&r);
    }

    // --- every code renders its OWN words ----------------------------------
    // Exact strings, not "at least three characters". An earlier version of
    // this asked only that something non-empty came back, which the
    // fallthrough `return "unclassified failure"` satisfies for ANY code — so
    // it would have passed for a code with no case at all, and swapping two
    // messages did not fail it either. Review caught both.
    {
        struct { AxlJsonErrorCode code; const char *want; } row[] = {
            { AXL_JSON_ERR_UNKNOWN,          "1:1: unclassified failure" },
            { AXL_JSON_ERR_INCOMPLETE,       "1:1: input ended early" },
            { AXL_JSON_ERR_UNEXPECTED_BYTE,  "1:1: unexpected byte" },
            { AXL_JSON_ERR_BAD_ESCAPE,       "1:1: bad string escape" },
            { AXL_JSON_ERR_BAD_NUMBER,       "1:1: bad number" },
            { AXL_JSON_ERR_BAD_UTF8,         "1:1: ill-formed UTF-8" },
            { AXL_JSON_ERR_DEPTH,            "1:1: nesting too deep" },
            { AXL_JSON_ERR_TRAILING,         "1:1: trailing content" },
            { AXL_JSON_ERR_DUPLICATE_KEY,    "1:1: duplicate key" },
            { AXL_JSON_ERR_IO,               "1:1: I/O failure" },
            { AXL_JSON_ERR_NO_MEMORY,        "1:1: out of memory" },
            { AXL_JSON_ERR_WRITER_STATE,     "1:1: writer used out of order" },
            { AXL_JSON_ERR_INVALID_ARGUMENT, "1:1: invalid argument" },
            { AXL_JSON_ERR_TRUNCATED,        "1:1: value did not fit" },
        };
        size_t i;
        bool   all_exact = true;

        for (i = 0; i < sizeof(row) / sizeof(row[0]); i++) {
            AxlJsonError e = { row[i].code, 0, 1, 1, 0 };
            char         buf[128];

            if (axl_json_error_format(&e, NULL, 0, buf, sizeof(buf)) <= 0
                || axl_strcmp(buf, row[i].want) != 0) {
                all_exact = false;
            }
        }
        test_check(all_exact,
                   "errfmt: every code renders its own exact words — a "
                   "swapped or missing message fails, not just a blank one");
    }

    // --- the buffer bound, swept -------------------------------------------
    // The single riskiest line in the formatter, and it had no test: an
    // off-by-one there is a real out-of-bounds write the whole suite passed
    // through. Sweeping every size up to just past the exact fit walks the
    // boundary instead of guessing at it.
    {
        const char   *doc = "{\"a\" 1}";
        AxlJsonReader r;
        char          full[128];
        int           want;
        size_t        size;
        bool          all_sane = true;

        test_check(!axl_json_parse(doc, axl_strlen(doc),
                                         AXL_JSON_STRICT, &r),
                   "errfmt: the bound-sweep document fails to parse");
        want = axl_json_error_format(axl_json_reader_error(&r), doc,
                                     axl_strlen(doc), full, sizeof(full));

        for (size = 1; size <= (size_t)want + 2; size++) {
            char   buf[128];
            size_t k;
            int    n;

            axl_memset(buf, (char)0xAA, sizeof(buf));
            n = axl_json_error_format(axl_json_reader_error(&r), doc,
                                      axl_strlen(doc), buf, size);
            if (size < (size_t)want + 1) {
                /* Must refuse — and still be printable, which is where this
                   deliberately differs from its two siblings. */
                if (n != -1 || buf[0] != '\0') {
                    all_sane = false;
                }
            } else if (n != want || axl_strcmp(buf, full) != 0) {
                all_sane = false;
            }
            for (k = size; k < sizeof(buf); k++) {
                if (buf[k] != (char)0xAA) {
                    all_sane = false;   /* wrote past the size it was given */
                }
            }
        }
        test_check(all_sane,
                   "errfmt: at every size 1..n+2 it refuses below the exact "
                   "fit, succeeds at it, and never writes past the size");
        axl_json_free(&r);
    }

    // --- AXL_JSON_ERROR_BUF_MAX is a size that cannot fail -----------------
    // Because the contract refuses rather than truncating, a caller otherwise
    // has no way to pick a size that is guaranteed to work — and the widest
    // render is a full window of 4-byte characters.
    {
        AXL_AUTOPTR(AxlString) doc = axl_string_new(NULL);
        AxlJsonReader          r;
        char                   buf[AXL_JSON_ERROR_BUF_MAX];
        size_t                 i;

        axl_string_append(doc, "{\"");
        for (i = 0; i < 120; i++) {
            axl_string_append(doc, "\xF0\x9F\x98\x80");
        }
        axl_string_append(doc, "\" 1}");
        test_check(!axl_json_parse(axl_string_str(doc),
                                         axl_strlen(axl_string_str(doc)),
                                         AXL_JSON_STRICT, &r),
                   "errfmt: the widest document fails to parse");
        test_check(axl_json_error_format(axl_json_reader_error(&r),
                                         axl_string_str(doc),
                                         axl_strlen(axl_string_str(doc)),
                                         buf, sizeof(buf)) > 0,
                   "errfmt: a buffer of AXL_JSON_ERROR_BUF_MAX never returns "
                   "-1, even on a full window of 4-byte characters");
        axl_json_free(&r);
    }

    // --- a non-NULL document with len 0 is the terse form ------------------
    {
        AxlJsonError e = { AXL_JSON_ERR_INCOMPLETE, 0, 2, 3, 0 };
        char         buf[64];

        test_check(axl_json_error_format(&e, "{}", 0, buf, sizeof(buf)) > 0
                   && axl_strcmp(buf, "2:3: input ended early") == 0,
                   "errfmt: len 0 is treated like a NULL document");
    }

    // --- a column past the end of the line still gets its caret ------------
    // Not reachable from a parser-produced record, but a caller may build one,
    // and the padding loop that handles it had no test.
    {
        AxlJsonError e = { AXL_JSON_ERR_INCOMPLETE, 3, 1, 8, 0 };
        char         buf[64];

        test_check(axl_json_error_format(&e, "{}", 2, buf, sizeof(buf)) > 0
                   && axl_strcmp(buf, "1:8: input ended early\n{}\n       ^")
                      == 0,
                   "errfmt: a column past end-of-line pads out to it rather "
                   "than stopping short");
    }

    // --- an offset past the document is clamped ----------------------------
    // The backing array deliberately runs PAST @a len and holds newlines
    // there. Without the clamp the line scan walks into them, picks a line
    // start beyond the document and quotes nothing — and, in a real caller,
    // reads memory it was never given. A document that simply ends at len
    // cannot show this: there is nothing past it to misread.
    {
        const char   backing[] = "{}\n\nXX";
        AxlJsonError e = { AXL_JSON_ERR_INCOMPLETE, 5, 1, 3, 0 };
        char         buf[64];

        test_check(axl_json_error_format(&e, backing, 2, buf, sizeof(buf)) > 0
                   && axl_strcmp(buf, "1:3: input ended early\n{}\n  ^") == 0,
                   "errfmt: an offset past @a len is clamped to it — the scan "
                   "never looks at bytes beyond the document");
    }

    // --- argument handling --------------------------------------------------
    {
        AxlJsonError e = { AXL_JSON_ERR_INCOMPLETE, 0, 1, 1, 0 };
        char         buf[64];
        char         tiny[4];

        test_check(axl_json_error_format(NULL, NULL, 0, buf, sizeof(buf)) == -1
                   && axl_json_error_format(&e, NULL, 0, NULL,
                                            sizeof(buf)) == -1
                   && axl_json_error_format(&e, NULL, 0, buf, 0) == -1,
                   "errfmt: NULL arguments and a zero-sized buffer are "
                   "refused");
        tiny[0] = (char)0xAA;
        test_check(axl_json_error_format(&e, NULL, 0, tiny, sizeof(tiny)) == -1
                   && tiny[0] == '\0',
                   "errfmt: a buffer too small is REFUSED, not truncated — "
                   "and left empty rather than partial");
    }
}

static void
test_json_writer_utf8_mode(void)
{
    // --- REPAIR is the default and is what the writer always did ----------
    wr_utf8_check(AXL_JSON_STRICT, "a\x80z", "{\"k\":\"a\xEF\xBF\xBDz\"}",
                  "wr utf8: REPAIR substitutes U+FFFD — the writer's standing "
                  "guarantee that its output is well-formed");

    // --- RAW copies the byte out verbatim ---------------------------------
    // The point of the mode: a field carrying firmware bytes round-trips.
    // The result is deliberately NOT well-formed JSON text, which is why it
    // has to be asked for by name.
    wr_utf8_check(AXL_JSON_UTF8_RAW, "a\x80z", "{\"k\":\"a\x80z\"}",
                  "wr utf8: RAW writes the ill-formed byte out as it came in");
    wr_utf8_check(AXL_JSON_UTF8_RAW, "caf\xC3\xA9",
                  "{\"k\":\"caf\xC3\xA9\"}",
                  "wr utf8: and leaves well-formed input alone, as every mode "
                  "does");

    // --- RAW cannot survive ENSURE_ASCII ----------------------------------
    // Escaping to \uXXXX needs a CODE POINT, and an ill-formed byte has
    // none. The two flags are in direct conflict and ENSURE_ASCII wins,
    // because its guarantee is the one that would otherwise be silently
    // broken. Documented, and pinned here so it cannot drift into "RAW wins".
    wr_utf8_check(AXL_JSON_UTF8_RAW | AXL_JSON_ENSURE_ASCII, "a\x80z",
                  "{\"k\":\"a\\ufffdz\"}",
                  "wr utf8: RAW + ENSURE_ASCII escapes as \\ufffd — there is "
                  "no code point to escape, so RAW cannot win");

    // --- STRICT refuses ----------------------------------------------------
    {
        AXL_AUTOPTR(AxlString) out = axl_string_new(NULL);
        AxlJsonWriter          w;

        axl_json_writer_init(&w, out, AXL_JSON_UTF8_STRICT);
        axl_json_obj_begin(&w);
        axl_json_kv_str(&w, "k", "a\x80z");
        axl_json_obj_end(&w);
        axl_json_writer_finish(&w);
        test_check(axl_json_writer_error(&w)
                   && axl_json_writer_error_info(&w)->code
                      == AXL_JSON_ERR_BAD_UTF8,
                   "wr utf8: STRICT sets the sticky error rather than writing "
                   "something it cannot vouch for");
    }
    {
        AXL_AUTOPTR(AxlString) out = axl_string_new(NULL);
        AxlJsonWriter          w;

        axl_json_writer_init(&w, out, AXL_JSON_UTF8_STRICT);
        axl_json_obj_begin(&w);
        axl_json_kv_str(&w, "k", "caf\xC3\xA9");
        axl_json_obj_end(&w);
        axl_json_writer_finish(&w);
        test_check(!axl_json_writer_error(&w)
                   && axl_strcmp(axl_string_str(out),
                                 "{\"k\":\"caf\xC3\xA9\"}") == 0,
                   "wr utf8: STRICT passes well-formed input through "
                   "untouched — it is a check, not a transform");
    }

    // --- the RESERVED field value is refused, not read as REPAIR -----------
    // RAW is 1 and STRICT is 2, so naming both ORs to the reserved value by
    // accident rather than by inventing a constant. Accepting it would
    // silently mean REPAIR — the mode disabled by the act of asking for two.
    // The reader refuses it at parse; the writer must agree at init.
    {
        AXL_AUTOPTR(AxlString) out = axl_string_new(NULL);
        AxlJsonWriter          w;

        axl_json_writer_init(&w, out,
                             AXL_JSON_UTF8_RAW | AXL_JSON_UTF8_STRICT);
        test_check(axl_json_writer_error(&w)
                   && axl_json_writer_error_info(&w)->code
                      == AXL_JSON_ERR_INVALID_ARGUMENT,
                   "wr utf8: writer_init refuses the reserved UTF-8 field "
                   "value — one rule for the reader and the writer");
    }
}

static void
test_json_ensure_ascii(void)
{
    // --- the BMP boundary, exact ------------------------------------------
    ascii_check("\xEF\xBF\xBF", "{\"k\":\"\\uffff\"}",
                "ensure_ascii: U+FFFF is a SINGLE escape — the last code "
                "point that fits in one");
    ascii_check("\xF0\x90\x80\x80", "{\"k\":\"\\ud800\\udc00\"}",
                "ensure_ascii: U+10000 is the FIRST surrogate pair");
    ascii_check("\xF4\x8F\xBF\xBF", "{\"k\":\"\\udbff\\udfff\"}",
                "ensure_ascii: U+10FFFF is the maximum pair");
    ascii_check("\xF0\x9F\x98\x80", "{\"k\":\"\\ud83d\\ude00\"}",
                "ensure_ascii: U+1F600 emoji becomes its documented pair");

    // --- the low boundary and the 2/3-byte forms --------------------------
    ascii_check("\xC2\x80", "{\"k\":\"\\u0080\"}",
                "ensure_ascii: U+0080, the first non-ASCII code point");
    ascii_check("\xC3\xA9", "{\"k\":\"\\u00e9\"}",
                "ensure_ascii: a 2-byte code point, lowercase hex");
    ascii_check("\xE4\xB8\xAD", "{\"k\":\"\\u4e2d\"}",
                "ensure_ascii: a 3-byte CJK code point");

    // --- pure ASCII is byte-identical with and without the flag -----------
    {
        AXL_AUTOPTR(AxlString) with    = axl_string_new(NULL);
        AXL_AUTOPTR(AxlString) without = axl_string_new(NULL);
        AxlJsonWriter          w;

        axl_json_writer_init(&w, with, AXL_JSON_ENSURE_ASCII);
        axl_json_obj_begin(&w);
        axl_json_kv_str(&w, "a/b", "plain \"quoted\"\ttext");
        axl_json_obj_end(&w);
        axl_json_writer_finish(&w);

        axl_json_writer_init(&w, without, AXL_JSON_STRICT);
        axl_json_obj_begin(&w);
        axl_json_kv_str(&w, "a/b", "plain \"quoted\"\ttext");
        axl_json_obj_end(&w);
        axl_json_writer_finish(&w);

        test_check(axl_strcmp(axl_string_str(with),
                              axl_string_str(without)) == 0,
                   "ensure_ascii: pure ASCII input is byte-identical with and "
                   "without the flag");
    }

    // --- KEYS are escaped too ---------------------------------------------
    {
        AXL_AUTOPTR(AxlString) out = axl_string_new(NULL);
        AxlJsonWriter          w;

        axl_json_writer_init(&w, out, AXL_JSON_ENSURE_ASCII);
        axl_json_obj_begin(&w);
        axl_json_kv_int(&w, "\xC3\xA9", 1);
        axl_json_obj_end(&w);
        axl_json_writer_finish(&w);
        test_check(axl_strcmp(axl_string_str(out), "{\"\\u00e9\":1}") == 0,
                   "ensure_ascii: an object KEY is escaped like any other "
                   "string");
    }

    // --- ill-formed input escapes as \ufffd -------------------------------
    // The writer repairs to U+FFFD first, so this is that rule seen through
    // the flag rather than a second one.
    ascii_check("a\xFFz", "{\"k\":\"a\\ufffdz\"}",
                "ensure_ascii: an ill-formed byte becomes the ESCAPED "
                "replacement, not a raw one");

    // --- the SPLICE path, and an escape already in the source -------------
    {
        const char            *doc = "{\"k\":\"\xC3\xA9 and \\u00e9\"}";
        AXL_AUTOPTR(AxlString) out = axl_string_new(NULL);
        AxlJsonReader          r;
        AxlJsonWriter          w;

        test_check(axl_json_parse(doc, axl_strlen(doc), AXL_JSON_RELAXED, &r),
                   "ensure_ascii: splice source parses");
        axl_json_writer_init(&w, out, AXL_JSON_ENSURE_ASCII);
        axl_json_write_token(&w, &r, 0);
        axl_json_writer_finish(&w);
        test_check(axl_strcmp(axl_string_str(out),
                              "{\"k\":\"\\u00e9 and \\u00e9\"}") == 0,
                   "ensure_ascii: reaches write_token — a raw code point is "
                   "escaped and one already escaped is left alone");
        axl_json_free(&r);
    }

    // --- a JSON5 `\<non-ASCII>` escape on the splice path -----------------
    //
    // The case above uses `\u00e9`, an escape made entirely of ASCII, which
    // is why it never caught this: the writer copies an ASCII run verbatim
    // and the pair survives by accident. Under ALLOW_EXTRA_ESCAPES the
    // payload can be a RAW multi-byte character instead, and then the
    // backslash is an escape INTRODUCER whose payload ENSURE_ASCII is about
    // to rewrite. Emitting the introducer and then re-encoding the payload
    // separately turns `\<char>` into `\\uXXXX` — a literal backslash
    // followed by text — which is a different value.
    //
    // Found by test/fuzz/json_fuzz's representation oracle, not by hand.
    {
        /* `\` + a raw 2-byte character. */
        const char            *doc = "{\"k\":\"\\\xC3\xA9\"}";
        AXL_AUTOPTR(AxlString) out = axl_string_new(NULL);
        AxlJsonReader          r;
        AxlJsonWriter          w;
        char                   val[16];

        test_check(axl_json_parse(doc, axl_strlen(doc),
                                        AXL_JSON_JSON5, &r),
                   "ensure_ascii: `\\<2-byte char>` source parses under JSON5");
        axl_json_writer_init(&w, out, AXL_JSON_ENSURE_ASCII);
        axl_json_write_token(&w, &r, 0);
        axl_json_writer_finish(&w);
        test_check(axl_strcmp(axl_string_str(out), "{\"k\":\"\\u00e9\"}") == 0,
                   "ensure_ascii: a `\\<raw char>` escape re-encodes as ONE "
                   "escape, not a literal backslash plus text");
        axl_json_free(&r);

        /* And it still means the same character. */
        test_check(axl_json_parse(axl_string_str(out),
                                        axl_string_len(out),
                                        AXL_JSON_JSON5, &r),
                   "ensure_ascii: `\\<2-byte char>` output re-parses");
        test_check(axl_json_get_string(&r, "k", val, sizeof(val))
                       && axl_strcmp(val, "\xC3\xA9") == 0,
                   "ensure_ascii: `\\<raw char>` still decodes to that "
                   "character after the round trip");
        axl_json_free(&r);
    }

    // --- a SINGLE-QUOTED source token spliced into a double-quoted one -----
    //
    // The splice re-quotes with `"` but copies the content verbatim, and
    // inside single quotes a `"` needs no escape — so `{a:'v"w'}` came out as
    // {"a":"v"w"}, which is not JSON in any dialect. The mirror case is `\'`,
    // an escape that is legal inside single quotes and invalid in a strict
    // double-quoted string.
    //
    // Written STRICT deliberately: the point is that a JSON5 document can be
    // converted to strict JSON, which is the main reason to re-emit at all.
    // Found by test/fuzz/json_fuzz's round-trip oracle.
    {
        const struct {
            const char *doc;
            const char *want;
            const char *msg;
        } rows[] = {
            { "{a:'v\"w'}", "{\"a\":\"v\\\"w\"}",
              "splice: a double quote inside a SINGLE-quoted value is escaped "
              "when re-emitted between double quotes" },
            { "{'k\"x':1}", "{\"k\\\"x\":1}",
              "splice: the same for a single-quoted KEY" },
            { "{a:'v\\'w'}", "{\"a\":\"v'w\"}",
              "splice: `\\'` loses an escape that only single quotes needed, "
              "rather than emitting one strict JSON rejects" },
        };

        for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
            AXL_AUTOPTR(AxlString) out = axl_string_new(NULL);
            AxlJsonReader          r;
            AxlJsonWriter          w;

            if (!axl_json_parse(rows[i].doc, axl_strlen(rows[i].doc),
                                      AXL_JSON_JSON5, &r)) {
                /* Both arms must emit the SAME number of assertions, or a
                   parse regression would move the pass count instead of
                   failing outright. */
                test_check(false, rows[i].msg);
                test_check(false,
                           "splice: the re-emitted document parses as STRICT "
                           "JSON");
                continue;
            }
            axl_json_writer_init(&w, out, AXL_JSON_STRICT);
            axl_json_write_token(&w, &r, 0);
            axl_json_writer_finish(&w);
            axl_json_free(&r);

            test_check(axl_strcmp(axl_string_str(out), rows[i].want) == 0,
                       rows[i].msg);

            /* And the result must be readable as STRICT JSON, which is the
               property the exact string above is a proxy for. */
            AxlJsonReader back;
            test_check(axl_json_parse(axl_string_str(out),
                                            axl_string_len(out),
                                            AXL_JSON_STRICT, &back),
                       "splice: the re-emitted document parses as STRICT JSON");
            axl_json_free(&back);
        }
    }

    // --- the same, but four bytes, which is where it also SPLIT ------------
    //
    // ESCAPE_SLASH pairs the backslash with the next BYTE. On a 4-byte
    // character that cuts the sequence after the lead byte, so the three
    // continuation bytes are orphaned and each repairs to U+FFFD — and the
    // lead byte is emitted RAW, which breaks ENSURE_ASCII's one promise.
    // Both flag settings must produce the same single escape pair.
    {
        const char *doc = "{\"k\":\"\\\xF0\x9F\x8C\x80\"}";
        const AxlJsonFlags rows[] = {
            AXL_JSON_ENSURE_ASCII,
            AXL_JSON_ENSURE_ASCII | AXL_JSON_ESCAPE_SLASH,
        };

        for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
            AXL_AUTOPTR(AxlString) out = axl_string_new(NULL);
            AxlJsonReader          r;
            AxlJsonWriter          w;

            test_check(axl_json_parse(doc, axl_strlen(doc),
                                            AXL_JSON_JSON5, &r),
                       "ensure_ascii: `\\<4-byte char>` source parses");
            axl_json_writer_init(&w, out, rows[i]);
            axl_json_write_token(&w, &r, 0);
            axl_json_writer_finish(&w);
            axl_json_free(&r);

            test_check(axl_strcmp(axl_string_str(out),
                                  "{\"k\":\"\\ud83c\\udf00\"}") == 0,
                       i == 0
                           ? "ensure_ascii: `\\<4-byte char>` becomes one "
                             "surrogate PAIR"
                           : "ensure_ascii+escape_slash: the backslash pairs "
                             "with the whole CHARACTER, not one byte");

            /* ENSURE_ASCII's entire promise, asserted directly: a raw lead
               byte leaking out is exactly what the byte-wise pairing did. */
            bool pure = true;
            for (size_t b = 0; b < axl_string_len(out); b++) {
                if ((unsigned char)axl_string_str(out)[b] >= 0x80) {
                    pure = false;
                }
            }
            test_check(pure,
                       i == 0 ? "ensure_ascii: output is pure ASCII"
                              : "ensure_ascii+escape_slash: output is pure "
                                "ASCII — no raw lead byte escapes");
        }
    }

    // --- and the result still round-trips to the same TEXT ----------------
    {
        AXL_AUTOPTR(AxlString) out = axl_string_new(NULL);
        AxlJsonWriter          w;
        AxlJsonReader          r;
        char                   buf[32];

        axl_json_writer_init(&w, out, AXL_JSON_ENSURE_ASCII);
        axl_json_obj_begin(&w);
        axl_json_kv_str(&w, "k", "\xF0\x9F\x98\x80");
        axl_json_obj_end(&w);
        axl_json_writer_finish(&w);

        test_check(axl_json_parse(axl_string_str(out),
                                  axl_strlen(axl_string_str(out)), AXL_JSON_RELAXED, &r)
                   && axl_json_get_string(&r, "k", buf, sizeof(buf))
                   && axl_strcmp(buf, "\xF0\x9F\x98\x80") == 0,
                   "ensure_ascii: the escaped pair decodes back to the SAME "
                   "4-byte code point — escaping is lossless");
        axl_json_free(&r);
    }
}

// ---------------------------------------------------------------------------
// SORT_KEYS (P6) — write_token only; the streaming writer buffers nothing
// ---------------------------------------------------------------------------

/* Parse @a doc in @a dialect and re-emit the whole thing through
   axl_json_write_token under @a wflags, comparing the WHOLE document.
   write_token is the only path SORT_KEYS reaches, so every ordering
   assertion goes through here. */
static void
sort_check(const char *doc, AxlJsonFlags dialect, AxlJsonFlags wflags,
           const char *want, const char *msg)
{
    AXL_AUTOPTR(AxlString) out = axl_string_new(NULL);
    AxlJsonReader          r;
    AxlJsonWriter          w;

    if (!axl_json_parse(doc, axl_strlen(doc), dialect, &r)) {
        test_check(false, msg);
        return;
    }
    axl_json_writer_init(&w, out, wflags);
    axl_json_write_token(&w, &r, 0);
    axl_json_writer_finish(&w);
    test_check(!axl_json_writer_error(&w)
               && axl_strcmp(axl_string_str(out), want) == 0, msg);
    axl_json_free(&r);
}

static void
test_json_sort_keys(void)
{
    // --- the baseline: WITHOUT the flag, source order survives -------------
    // Asserted first and separately. Every expectation below is "sorted"
    // rather than "different", and that only means something if the unsorted
    // path is known to preserve what the document said.
    sort_check("{\"c\":1,\"a\":2,\"b\":3}", AXL_JSON_STRICT, AXL_JSON_STRICT,
               "{\"c\":1,\"a\":2,\"b\":3}",
               "sort: without SORT_KEYS the source order is preserved");

    // --- flat, and the nested object that proves it recurses ---------------
    sort_check("{\"c\":1,\"a\":2,\"b\":3}", AXL_JSON_STRICT,
               AXL_JSON_SORT_KEYS, "{\"a\":2,\"b\":3,\"c\":1}",
               "sort: a flat object comes out in key order");
    sort_check("{\"z\":{\"q\":1,\"b\":2},\"a\":3}", AXL_JSON_STRICT,
               AXL_JSON_SORT_KEYS, "{\"a\":3,\"z\":{\"b\":2,\"q\":1}}",
               "sort: a NESTED object is sorted too — sorting recurses");
    /* The nested container LAST as well as first: a container at the end is
       where a skip-loop bug hides, because the member count ends the walk
       whatever the skip does. */
    sort_check("{\"b\":{\"y\":1,\"x\":2},\"a\":0}", AXL_JSON_STRICT,
               AXL_JSON_SORT_KEYS, "{\"a\":0,\"b\":{\"x\":2,\"y\":1}}",
               "sort: and when the nested object sorts to the END, where a "
               "mis-skipped subtree would not be noticed");
    sort_check("[{\"b\":1,\"a\":2},{\"d\":3,\"c\":4}]", AXL_JSON_STRICT,
               AXL_JSON_SORT_KEYS, "[{\"a\":2,\"b\":1},{\"c\":4,\"d\":3}]",
               "sort: objects INSIDE an array are sorted, each on its own");

    // --- what must NOT be reordered ----------------------------------------
    // TWO members, deliberately. A one-member object never reaches the sorted
    // path at all (the `size > 1` early-out), so `{"k":[3,1,2]}` would assert
    // nothing about SORT_KEYS — it would exercise the unsorted walk and pass
    // however the sorted one behaved. A review caught exactly that.
    sort_check("{\"k\":[3,1,2],\"a\":0}", AXL_JSON_STRICT, AXL_JSON_SORT_KEYS,
               "{\"a\":0,\"k\":[3,1,2]}",
               "sort: array elements keep their order — that is data, not "
               "key order");

    // --- the sorted walk must SKIP a subtree it does not sort ---------------
    // These are what cover token_subtree_end's array arm. Without them the
    // whole arm could be deleted and every other case here still passes: the
    // member walk only has to step over an array when one is the value of an
    // object big enough to sort.
    sort_check("{\"b\":[1,[2,3],4],\"a\":0}", AXL_JSON_STRICT,
               AXL_JSON_SORT_KEYS, "{\"a\":0,\"b\":[1,[2,3],4]}",
               "sort: a NESTED array inside a sorted object is stepped over "
               "whole — the member after it is still found");
    sort_check("{\"b\":[{\"y\":1,\"x\":2}],\"a\":0}", AXL_JSON_STRICT,
               AXL_JSON_SORT_KEYS, "{\"a\":0,\"b\":[{\"x\":2,\"y\":1}]}",
               "sort: an object inside an array inside a sorted object is "
               "itself sorted");
    sort_check("{\"b\":[],\"a\":0}", AXL_JSON_STRICT, AXL_JSON_SORT_KEYS,
               "{\"a\":0,\"b\":[]}",
               "sort: an EMPTY array value — the zero-trip skip loop");

    // --- an ILL-FORMED key sorts by the name it will actually carry --------
    //
    // The docstring promises order over the DECODED name, and only ESCAPED
    // keys were being decoded. A key holding a raw ill-formed byte is not
    // escaped, so it was sorted by source bytes — while being EMITTED with
    // UTF-8 repair applied. Those disagree: 0xFF sorts AFTER 0xF0 as a raw
    // byte, but its repaired name U+FFFD (EF BF BD) sorts BEFORE it.
    //
    // The visible consequence is that sorting stopped being idempotent —
    // re-sorting the writer's own output produced a different order — which
    // is the one thing this flag exists to guarantee. Found by
    // test/fuzz/json_fuzz's round-trip oracle.
    sort_check("{\"\xFF\":1,\"\xF0\x9F\x98\x80\":2}", AXL_JSON_STRICT,
               AXL_JSON_SORT_KEYS,
               "{\"\xEF\xBF\xBD\":1,\"\xF0\x9F\x98\x80\":2}",
               "sort: an ill-formed key orders by its REPAIRED name, the one "
               "it is emitted under");

    // A key whose decode GROWS. JSON5's `\0` is two source bytes and names
    // U+FFFD (three), so a sort buffer sized for "decoding never shrinks or
    // grows" overflows or refuses. Sizing it at len+1 made SORT_KEYS fail
    // outright on this document — a regression caught by review, not by the
    // fuzzer, whose round-trip oracle skips any document the writer errors on.
    // U+FFFD (EF BF BD) sorts AFTER `z`, which is the decoded-name order.
    sort_check("{'\\0':1,'z':2}", AXL_JSON_JSON5, AXL_JSON_SORT_KEYS,
               "{\"z\":2,\"\\0\":1}",
               "sort: a key whose decode GROWS still fits its buffer, and "
               "orders by the name it grew into");

    // Under UTF8_RAW no repair happens, so the raw bytes ARE the emitted
    // bytes and sorting by them is correct — 0xFF sorts last. Pins that the
    // fix is mode-aware rather than an unconditional decode.
    sort_check("{\"\xFF\":1,\"\xF0\x9F\x98\x80\":2}",
               AXL_JSON_STRICT | AXL_JSON_UTF8_RAW,
               AXL_JSON_SORT_KEYS | AXL_JSON_UTF8_RAW,
               "{\"\xF0\x9F\x98\x80\":2,\"\xFF\":1}",
               "sort: under UTF8_RAW the raw bytes are the emitted bytes, so "
               "they are what orders");
    sort_check("{\"b\":{},\"a\":0}", AXL_JSON_STRICT, AXL_JSON_SORT_KEYS,
               "{\"a\":0,\"b\":{}}",
               "sort: an EMPTY object value — the same, on the object arm");

    // --- keys differing only in case ---------------------------------------
    // Byte order, so the whole uppercase run precedes the whole lowercase
    // one. A case-FOLDING sort would interleave them as A,a,B,b and a
    // case-insensitive one would leave the pairs in source order.
    sort_check("{\"b\":1,\"A\":2,\"a\":3,\"B\":4}", AXL_JSON_STRICT,
               AXL_JSON_SORT_KEYS, "{\"A\":2,\"B\":4,\"a\":3,\"b\":1}",
               "sort: case is not folded — every uppercase key precedes "
               "every lowercase one");

    // --- sorted by the DECODED name, not the source spelling ---------------
    // The discriminating case for the whole decode path. `\u0062` NAMES `b`,
    // so it must sort AFTER `a`. Compared as source bytes it would sort
    // FIRST, because `\` is 0x5C and `a` is 0x61 — and the document would
    // come out in its original order, looking untouched rather than wrong.
    sort_check("{\"\\u0062\":1,\"a\":2}", AXL_JSON_STRICT, AXL_JSON_SORT_KEYS,
               "{\"a\":2,\"\\u0062\":1}",
               "sort: an escaped key sorts by its DECODED name, and re-emits "
               "in its original source spelling");
    /* Source deliberately NOT already in order: an input that arrives sorted
       passes whether or not the flag does anything. */
    sort_check("{\"\\u0043\":3,\"B\":2,\"\\u0041\":1}", AXL_JSON_STRICT,
               AXL_JSON_SORT_KEYS, "{\"\\u0041\":1,\"B\":2,\"\\u0043\":3}",
               "sort: escaped and plain keys interleave by decoded name — "
               "A, B, C from two different spellings");

    // --- a prefix sorts before what extends it -----------------------------
    sort_check("{\"ab\":1,\"a\":2,\"abc\":3}", AXL_JSON_STRICT,
               AXL_JSON_SORT_KEYS, "{\"a\":2,\"ab\":1,\"abc\":3}",
               "sort: a key that is a PREFIX of another sorts first");

    // --- duplicate keys keep their source order ----------------------------
    // Three duplicates with DISTINCT values, so a reordering is visible;
    // two would have a 50% chance of looking correct by luck.
    sort_check("{\"b\":9,\"a\":1,\"a\":2,\"a\":3}", AXL_JSON_STRICT,
               AXL_JSON_SORT_KEYS, "{\"a\":1,\"a\":2,\"a\":3,\"b\":9}",
               "sort: duplicate keys are all kept, in the order the document "
               "listed them");

    /* ...but that case CANNOT exercise the comparator's tie-break, and a
       sabotage proved it: axl_qsort runs pure insertion sort at or below
       INSERTION_THRESHOLD (16) and insertion sort is stable, so any object
       small enough to read comfortably keeps source order whether the
       tie-break is there or not. Deleting the tie-break left the case above
       green.

       Twenty members past that threshold, every one of them the SAME key,
       so the run reaches introsort's partitioning — where equal elements are
       swapped across the pivot and source order survives only because the
       comparator refuses to call any two members equal. Output must be
       byte-identical to input, which is why one string serves as both. */
    {
        const char *dup20 =
            "{\"k\":0,\"k\":1,\"k\":2,\"k\":3,\"k\":4,\"k\":5,\"k\":6,"
            "\"k\":7,\"k\":8,\"k\":9,\"k\":10,\"k\":11,\"k\":12,\"k\":13,"
            "\"k\":14,\"k\":15,\"k\":16,\"k\":17,\"k\":18,\"k\":19}";

        sort_check(dup20, AXL_JSON_STRICT, AXL_JSON_SORT_KEYS, dup20,
                   "sort: twenty identical keys past the insertion-sort "
                   "threshold keep source order — the tie-break, not luck");
    }

    // --- the degenerate sizes ----------------------------------------------
    sort_check("{}", AXL_JSON_STRICT, AXL_JSON_SORT_KEYS, "{}",
               "sort: an empty object is unchanged");
    sort_check("{\"a\":1}", AXL_JSON_STRICT, AXL_JSON_SORT_KEYS, "{\"a\":1}",
               "sort: a one-member object is unchanged");

    // --- fully reversed, wide enough that a partial sort would show ---------
    // NOT a collector-growth case: axl_array_sized_new reserves exactly the
    // member count and the array takes exactly that many appends, so the
    // grow path is unreachable for ANY input here.
    sort_check("{\"j\":9,\"i\":8,\"h\":7,\"g\":6,\"f\":5,\"e\":4,\"d\":3,"
               "\"c\":2,\"b\":1,\"a\":0}", AXL_JSON_STRICT,
               AXL_JSON_SORT_KEYS,
               "{\"a\":0,\"b\":1,\"c\":2,\"d\":3,\"e\":4,\"f\":5,\"g\":6,"
               "\"h\":7,\"i\":8,\"j\":9}",
               "sort: ten members in exactly reverse order come out exactly "
               "forward");

    // --- JSON5 unquoted keys sort too, and quote on the way out ------------
    sort_check("{c:1,a:2,b:3}", AXL_JSON_JSON5, AXL_JSON_SORT_KEYS,
               "{\"a\":2,\"b\":3,\"c\":1}",
               "sort: JSON5 unquoted keys sort by the same name and are "
               "quoted on output");

    // --- composes with the formatting flags ---------------------------------
    sort_check("{\"c\":1,\"a\":{\"y\":1,\"x\":2}}", AXL_JSON_STRICT,
               AXL_JSON_INDENT(2) | AXL_JSON_SORT_KEYS,
               "{\n  \"a\": {\n    \"x\": 2,\n    \"y\": 1\n  },\n  \"c\": 1\n}",
               "sort: INDENT(2) | SORT_KEYS — sorted AND correctly indented "
               "at every depth");

    // --- on a STREAMING write it is a documented no-op ----------------------
    // Pinned rather than assumed. Nothing is buffered, so there is nothing to
    // sort; the risk is a future implementation quietly acquiring an opinion
    // here, which this would catch.
    {
        AXL_AUTOPTR(AxlString) out = axl_string_new(NULL);
        AxlJsonWriter          w;

        axl_json_writer_init(&w, out, AXL_JSON_SORT_KEYS);
        axl_json_obj_begin(&w);
        axl_json_kv_int(&w, "c", 1);
        axl_json_kv_int(&w, "a", 2);
        axl_json_kv_int(&w, "b", 3);
        axl_json_obj_end(&w);
        axl_json_writer_finish(&w);
        test_check(!axl_json_writer_error(&w)
                   && axl_strcmp(axl_string_str(out),
                                 "{\"c\":1,\"a\":2,\"b\":3}") == 0,
                   "sort: a STREAMING write is emission-ordered — SORT_KEYS "
                   "is a no-op there, and stays one");
    }
}

// ---------------------------------------------------------------------------
// REJECT_DUPLICATES (P7) — reader-side, opt-in, by DECODED key name
// ---------------------------------------------------------------------------

/* Parse @a doc with @a flags and assert it FAILS with @a code at @a offset.
   Position is asserted, not just the code: "some error was reported" is the
   assertion this phase most needs not to make. */
static void
dup_reject_check(const char *doc, AxlJsonFlags flags, size_t offset,
                 const char *msg)
{
    AxlJsonReader       r;
    const bool          ok = axl_json_parse(doc, axl_strlen(doc),
                                                  flags, &r);
    const AxlJsonError *e  = axl_json_reader_error(&r);

    test_check(!ok && e->code == AXL_JSON_ERR_DUPLICATE_KEY
               && e->offset == offset, msg);
    axl_json_free(&r);
}

static void
test_json_reject_duplicates(void)
{
    // --- WITHOUT the flag: pin what a duplicate does today -----------------
    // Undocumented before this phase, and it is the behavior the flag opts
    // out of, so it is pinned first and separately. RFC 8259 §4 permits
    // repeated names; AXL accepts them.
    {
        AxlJsonReader r;
        int64_t       n = 0;

        test_check(axl_json_parse("{\"a\":1,\"a\":2}", 13, AXL_JSON_RELAXED, &r),
                   "dup: without the flag a repeated key PARSES — RFC 8259 "
                   "permits it");
        test_check(axl_json_get_int(&r, "a", &n) && n == 1,
                   "dup: and a by-key accessor returns the FIRST occurrence");
        axl_json_free(&r);

        /* The escaped spelling resolves to the same name, so this is the
           same document as far as any accessor is concerned. */
        test_check(axl_json_parse("{\"\\u0061\":1,\"a\":2}", 18, AXL_JSON_RELAXED, &r),
                   "dup: an ESCAPED duplicate parses too, without the flag");
        test_check(axl_json_get_int(&r, "a", &n) && n == 1,
                   "dup: and `\\u0061` is findable as `a` — the two keys name "
                   "the same thing");
        axl_json_free(&r);
    }

    // --- WITH the flag: the same documents are refused ----------------------
    // Offset is the SECOND key's first NAME byte — inside the quotes, not the
    // opening quote. A JSON5 unquoted key has no quote, and one rule that
    // holds for every spelling beats two that differ by a byte.
    dup_reject_check("{\"a\":1,\"a\":2}", AXL_JSON_REJECT_DUPLICATES, 8,
                     "dup: REJECT_DUPLICATES fails the parse, positioned at "
                     "the SECOND key");
    dup_reject_check("{\"\\u0061\":1,\"a\":2}", AXL_JSON_REJECT_DUPLICATES, 13,
                     "dup: an escaped key duplicates a plain one — compared "
                     "by DECODED name, so escaping does not evade the check");
    dup_reject_check("{\"a\":1,\"\\u0061\":2}", AXL_JSON_REJECT_DUPLICATES, 8,
                     "dup: and in the other direction, plain then escaped");

    // --- it is per OBJECT, and it reaches every depth -----------------------
    {
        AxlJsonReader r;

        test_check(axl_json_parse("{\"x\":{\"a\":1},\"y\":{\"a\":2}}", 25,
                                        AXL_JSON_REJECT_DUPLICATES, &r),
                   "dup: two SIBLING objects may each carry the same key — "
                   "the check is per object, not per document");
        axl_json_free(&r);
    }
    dup_reject_check("{\"x\":{\"a\":1,\"a\":2}}", AXL_JSON_REJECT_DUPLICATES,
                     13,
                     "dup: a duplicate NESTED one level down is still caught");
    dup_reject_check("{\"x\":[{\"a\":1,\"a\":2}]}", AXL_JSON_REJECT_DUPLICATES,
                     14,
                     "dup: and inside an object inside an array");

    // --- a member's VALUE is stepped over WHOLE -----------------------------
    // Both of these turn on the member walk skipping a subtree rather than
    // assuming two tokens per member. A sabotage replacing the skip with
    // `next += 2` passed every other case here, so these are what make the
    // walk load-bearing.
    {
        AxlJsonReader r;

        /* A nested object may reuse its PARENT's key: different objects,
           different scopes. A two-token stride would walk into the child and
           see the inner `a` as a second member of the outer object — a false
           duplicate in a document that is perfectly legal. */
        test_check(axl_json_parse("{\"a\":{\"a\":1},\"b\":2}", 19,
                                        AXL_JSON_REJECT_DUPLICATES, &r),
                   "dup: a nested object may REUSE its parent's key — scopes "
                   "are per object, not per path");
        axl_json_free(&r);
    }
    /* And the converse: a real duplicate sitting AFTER a multi-token value.
       A two-token stride lands on an array ELEMENT, which is not a string, so
       the walk gives up and the duplicate goes unreported. */
    dup_reject_check("{\"x\":[1,2],\"a\":3,\"a\":4}",
                     AXL_JSON_REJECT_DUPLICATES, 18,
                     "dup: a duplicate AFTER an array-valued member is still "
                     "found — the array is stepped over, not walked into");

    // --- what must still parse ---------------------------------------------
    {
        AxlJsonReader r;

        test_check(axl_json_parse("{\"a\":1,\"b\":2,\"c\":3}", 19,
                                        AXL_JSON_REJECT_DUPLICATES, &r),
                   "dup: distinct keys are unaffected");
        axl_json_free(&r);
        test_check(axl_json_parse("{}", 2,
                                        AXL_JSON_REJECT_DUPLICATES, &r),
                   "dup: an empty object is unaffected");
        axl_json_free(&r);
        /* Repeated VALUES are not repeated keys, and an array has no keys at
           all — a check that walked tokens rather than object members could
           confuse the two. */
        /* TWO members: a one-member object is short-circuited before the
           member walk runs, so it could not tell us anything about how array
           elements are treated. */
        test_check(axl_json_parse("{\"k\":[1,1,1],\"j\":[1,1]}", 23,
                                        AXL_JSON_REJECT_DUPLICATES, &r),
                   "dup: repeated array ELEMENTS are not duplicate keys");
        axl_json_free(&r);
        test_check(axl_json_parse("{\"a\":\"a\",\"b\":\"b\"}", 17,
                                        AXL_JSON_REJECT_DUPLICATES, &r),
                   "dup: a key equal to a sibling's VALUE is not a duplicate "
                   "— only keys are compared");
        axl_json_free(&r);
    }

    // --- only the FIRST duplicate is reported -------------------------------
    // A documented clause, and one a "report the last one found" bug would
    // satisfy just as well without the offset being asserted.
    dup_reject_check("{\"a\":1,\"b\":2,\"b\":3,\"a\":4}",
                     AXL_JSON_REJECT_DUPLICATES, 14,
                     "dup: with two duplicate pairs, the FIRST to complete is "
                     "reported — the second `b`, not the second `a`");
    dup_reject_check("{\"a\":1,\"a\":2,\"a\":3}", AXL_JSON_REJECT_DUPLICATES, 8,
                     "dup: three occurrences report the SECOND, not the last");

    // --- the degenerate key -------------------------------------------------
    dup_reject_check("{\"\":1,\"\":2}", AXL_JSON_REJECT_DUPLICATES, 7,
                     "dup: two EMPTY-string keys are duplicates — a zero "
                     "length must not read as 'no key'");

    // --- the outer walk keeps going past a CLEAN object ---------------------
    dup_reject_check("{\"x\":{\"a\":1,\"b\":2},\"y\":{\"c\":3,\"c\":4}}",
                     AXL_JSON_REJECT_DUPLICATES, 31,
                     "dup: a duplicate in the SECOND sibling object is found — "
                     "a clean first object does not end the search");

    // --- JSON5 unquoted keys: POSITION, not just the code -------------------
    // The unquoted spelling is the one the "first name byte, not the opening
    // quote" rule exists for, so it is the one whose offset most needs pinning.
    dup_reject_check("{a:1,a:2}", AXL_JSON_JSON5 | AXL_JSON_REJECT_DUPLICATES,
                     5,
                     "dup: an unquoted JSON5 key reports its own first byte — "
                     "there is no quote to point at");
    dup_reject_check("{a:1,\"a\":2}",
                     AXL_JSON_JSON5 | AXL_JSON_REJECT_DUPLICATES, 6,
                     "dup: and the quoted partner reports INSIDE its quotes");

    // --- both sides of the linear/hash threshold ----------------------------
    // Detection switches strategy at DUP_LINEAR_MAX (8) members and the table
    // resizes again past 48, so a single width would exercise one path and
    // silently leave the others unproven. Sweeping 2..60 members crosses both.
    {
        size_t width;
        bool   all_clean  = true;
        bool   all_caught = true;

        for (width = 2; width <= 60; width++) {
            AXL_AUTOPTR(AxlString) ok  = axl_string_new(NULL);
            AXL_AUTOPTR(AxlString) bad = axl_string_new(NULL);
            AxlJsonWriter          w;
            AxlJsonReader          r;
            size_t                 i;
            char                   key[16];

            /* distinct keys — must parse */
            axl_json_writer_init(&w, ok, AXL_JSON_STRICT);
            axl_json_obj_begin(&w);
            for (i = 0; i < width; i++) {
                axl_snprintf(key, sizeof(key), "k%02zu", i);
                axl_json_kv_uint(&w, key, (uint64_t)i);
            }
            axl_json_obj_end(&w);
            axl_json_writer_finish(&w);
            if (!axl_json_parse(axl_string_str(ok),
                                      axl_strlen(axl_string_str(ok)),
                                      AXL_JSON_REJECT_DUPLICATES, &r)) {
                all_clean = false;
            }
            axl_json_free(&r);

            /* same, with the LAST key repeating the first — the longest
               possible distance between the pair at this width */
            axl_json_writer_init(&w, bad, AXL_JSON_STRICT);
            axl_json_obj_begin(&w);
            for (i = 0; i < width; i++) {
                axl_snprintf(key, sizeof(key), "k%02zu",
                             i + 1 == width ? (size_t)0 : i);
                axl_json_kv_uint(&w, key, (uint64_t)i);
            }
            axl_json_obj_end(&w);
            axl_json_writer_finish(&w);
            if (axl_json_parse(axl_string_str(bad),
                                     axl_strlen(axl_string_str(bad)),
                                     AXL_JSON_REJECT_DUPLICATES, &r)
                || axl_json_reader_error(&r)->code
                   != AXL_JSON_ERR_DUPLICATE_KEY) {
                all_caught = false;
            }
            axl_json_free(&r);
        }
        test_check(all_clean,
                   "dup: distinct keys parse at every width 2..60 — across "
                   "the linear/hash switch and the table's resize");
        test_check(all_caught,
                   "dup: and a first/last repeat is caught at every one of "
                   "those widths");
    }

    // --- out of memory must fail the parse, never accept it -----------------
    // A strictness flag that fails OPEN is worse than one that is absent: the
    // caller believes the document was checked. Review found exactly that —
    // an unchecked axl_hash_table_add return let an OOM parse succeed AND
    // leak the key. Every allocation index is swept because the paths differ
    // (the hash table, its buckets, each decoded key), and any one of them
    // failing must give the same answer.
    {
        const char *doc = "{\"k00\":0,\"k01\":1,\"k02\":2,\"k03\":3,\"k04\":4,"
                          "\"k05\":5,\"k06\":6,\"k07\":7,\"k08\":8,\"k09\":9,"
                          "\"k00\":99}";
        unsigned    n;
        bool        never_accepted = true;

        for (n = 1; n <= 40; n++) {
            AxlJsonReader r;
            bool          ok;

            axl_mem_fail_next_alloc(n);
            ok = axl_json_parse(doc, axl_strlen(doc),
                                      AXL_JSON_REJECT_DUPLICATES, &r);
            axl_mem_fail_next_alloc(0);
            /* Either the allocation failure was reported, or the parse got
               far enough to see the duplicate. Never success. */
            if (ok) {
                never_accepted = false;
            }
            axl_json_free(&r);
        }
        test_check(never_accepted,
                   "dup: a document WITH a duplicate is never accepted, "
                   "whichever of the first 40 allocations fails");
    }

    // --- JSON5 unquoted keys are keys too -----------------------------------
    {
        AxlJsonReader r;

        test_check(!axl_json_parse("{a:1,a:2}", 9,
                                         AXL_JSON_JSON5
                                         | AXL_JSON_REJECT_DUPLICATES, &r)
                   && axl_json_reader_error(&r)->code
                      == AXL_JSON_ERR_DUPLICATE_KEY,
                   "dup: JSON5 unquoted keys duplicate by the same rule");
        axl_json_free(&r);
        /* A quoted key and an unquoted one naming the same thing. */
        test_check(!axl_json_parse("{a:1,\"a\":2}", 11,
                                         AXL_JSON_JSON5
                                         | AXL_JSON_REJECT_DUPLICATES, &r)
                   && axl_json_reader_error(&r)->code
                      == AXL_JSON_ERR_DUPLICATE_KEY,
                   "dup: across the quoted/unquoted spellings as well");
        axl_json_free(&r);
    }
}

// ---------------------------------------------------------------------------
// AXL_JSON_UTF8_REPAIR / _RAW on READ (P7) — accessor-time, DECODED bytes
// ---------------------------------------------------------------------------

/* Parse @a doc under @a mode and compare the whole decoded value of key "a".
   One helper for both modes, so a row can be stated as "these bytes in, those
   bytes out" without the mode changing anything else about the call. */
static void
utf8_read_check(const char *doc, AxlJsonFlags mode, const char *want,
                const char *msg)
{
    AxlJsonReader r;
    char          buf[64];

    if (!axl_json_parse(doc, axl_strlen(doc), AXL_JSON_JSON5 | mode,
                              &r)) {
        test_check(false, msg);
        return;
    }
    test_check(axl_json_get_string(&r, "a", buf, sizeof(buf))
               && axl_strcmp(buf, want) == 0, msg);
    axl_json_free(&r);
}

static void
test_json_utf8_repair_read(void)
{
    // --- RAW hands ill-formed bytes back; REPAIR substitutes ---------------
    // The same document under both modes, so the difference is the mode and
    // nothing else.
    utf8_read_check("{\"a\":\"x\x80y\"}", AXL_JSON_UTF8_RAW, "x\x80y",
                    "utf8 repair: RAW hands an orphan continuation back as "
                    "found");
    utf8_read_check("{\"a\":\"x\x80y\"}", AXL_JSON_UTF8_REPAIR,
                    "x\xEF\xBF\xBDy",
                    "utf8 repair: REPAIR turns it into U+FFFD");

    // REPAIR is the ZERO value, so naming no mode must behave as REPAIR.
    utf8_read_check("{\"a\":\"x\x80y\"}", 0, "x\xEF\xBF\xBDy",
                    "utf8 repair: and REPAIR is what naming no mode gets — "
                    "it is the zero value");

    // --- one replacement per ill-formed BYTE, resynchronising --------------
    utf8_read_check("{\"a\":\"\x80\x80\"}", AXL_JSON_UTF8_REPAIR,
                    "\xEF\xBF\xBD\xEF\xBF\xBD",
                    "utf8 repair: two bad bytes give two replacements, not "
                    "one run collapsed into one");
    utf8_read_check("{\"a\":\"\xC3\"}", AXL_JSON_UTF8_REPAIR,
                    "\xEF\xBF\xBD",
                    "utf8 repair: a truncated lead byte at end of string");
    utf8_read_check("{\"a\":\"\xC3z\"}", AXL_JSON_UTF8_REPAIR,
                    "\xEF\xBF\xBDz",
                    "utf8 repair: a lead with no continuation keeps the "
                    "character that followed it");

    // --- well-formed input is untouched under BOTH modes -------------------
    utf8_read_check("{\"a\":\"\xE2\x82\xAC\"}", AXL_JSON_UTF8_REPAIR,
                    "\xE2\x82\xAC",
                    "utf8 repair: a well-formed 3-byte sequence is not "
                    "touched by REPAIR");
    utf8_read_check("{\"a\":\"\xF0\x9F\x98\x80\"}", AXL_JSON_UTF8_REPAIR,
                    "\xF0\x9F\x98\x80",
                    "utf8 repair: nor a 4-byte one");

    // --- the ASSEMBLED characters must survive REPAIR ----------------------
    // This is why the mode is judged on DECODED bytes. JSON5 lets any byte be
    // escaped, so a character can arrive split across escapes and raw bytes.
    // A repair that looked at the SOURCE would see a lone lead in each of
    // these and destroy a character AXL assembles correctly.
    utf8_read_check("{\"a\":\"\\\xC3\\\xA9\"}", AXL_JSON_UTF8_REPAIR,
                    "\xC3\xA9",
                    "utf8 repair: two separately-escaped bytes still assemble "
                    "into one character under REPAIR");
    utf8_read_check("{\"a\":\"\xC3\\\x80\"}", AXL_JSON_UTF8_REPAIR,
                    "\xC3\x80",
                    "utf8 repair: a RAW lead plus an ESCAPED continuation "
                    "still assembles — judging the source would break it");
    utf8_read_check("{\"a\":\"\\\xF0\\\x9F\\\x98\\\x80\"}",
                    AXL_JSON_UTF8_REPAIR, "\xF0\x9F\x98\x80",
                    "utf8 repair: four separately-escaped bytes assemble into "
                    "one 4-byte character under REPAIR");

    // --- escapes keep their own rules --------------------------------------
    utf8_read_check("{\"a\":\"\\ud800\"}", AXL_JSON_UTF8_RAW,
                    "\xEF\xBF\xBD",
                    "utf8 repair: a lone surrogate ESCAPE is U+FFFD even "
                    "under RAW — that rule is not the UTF-8 mode's");
    utf8_read_check("{\"a\":\"\\xe9\"}", AXL_JSON_UTF8_REPAIR,
                    "\xC3\xA9",
                    "utf8 repair: \\xNN is still the code UNIT U+00NN, not a "
                    "raw byte to be repaired");

    // --- inheritance is asserted with RAW, which is NOT the zero value -----
    // REPAIR is 0, so asserting that a sub-reader REPAIRS proves nothing: the
    // destination is an uninitialised stack struct, and deleting the
    // inheritance line still passes whenever that slot happens to be zero.
    // A review caught exactly that. RAW is non-zero, so these fail unless the
    // mode was really carried across.
    {
        AxlJsonReader     r;
        AxlJsonReader     sub;
        AxlJsonObjectIter it;
        AxlJsonReader     val;
        AxlJsonArrayIter  ait;
        AxlJsonReader     elem;
        char              kbuf[32];
        char              vbuf[32];
        const char       *doc = "{\"o\":{\"a\":\"x\x80y\"}}";

        test_check(axl_json_parse(doc, axl_strlen(doc),
                                        AXL_JSON_UTF8_RAW, &r)
                   && axl_json_get_object(&r, "o", &sub)
                   && axl_json_get_string(&sub, "a", vbuf, sizeof(vbuf))
                   && axl_strcmp(vbuf, "x\x80y") == 0,
                   "utf8 repair: a SUB-READER inherits RAW — it does not fall "
                   "back to the zero value, which is REPAIR");
        axl_json_free(&r);

        doc = "{\"o\":{\"k\x80\":\"v\x80\"}}";
        test_check(axl_json_parse(doc, axl_strlen(doc),
                                        AXL_JSON_UTF8_RAW, &r)
                   && axl_json_object_begin(&r, "o", &it)
                   && axl_json_object_next(&it, kbuf, sizeof(kbuf), &val)
                   && axl_strcmp(kbuf, "k\x80") == 0
                   && axl_json_value_string(&val, vbuf, sizeof(vbuf))
                   && axl_strcmp(vbuf, "v\x80") == 0,
                   "utf8 repair: the OBJECT iterator inherits RAW, key and "
                   "value alike");
        axl_json_free(&r);

        doc = "{\"a\":[\"x\x80y\"]}";
        test_check(axl_json_parse(doc, axl_strlen(doc),
                                        AXL_JSON_UTF8_RAW, &r)
                   && axl_json_array_begin(&r, "a", &ait)
                   && axl_json_array_next(&ait, &elem)
                   && axl_json_value_string(&elem, vbuf, sizeof(vbuf))
                   && axl_strcmp(vbuf, "x\x80y") == 0,
                   "utf8 repair: and so does the ARRAY iterator");
        axl_json_free(&r);
    }

    // --- a repaired key is findable by the name iteration reported ----------
    // The contradiction token_equals exists to remove, in its UTF-8 form: the
    // by-key fast path compared raw bytes without consulting the mode, so a
    // key holding an ill-formed byte iterated as `k<U+FFFD>` and then could
    // not be looked up under that name. Whether it worked depended on how the
    // DOCUMENT happened to spell the byte, which is no rule at all.
    {
        AxlJsonReader     r;
        AxlJsonObjectIter it;
        AxlJsonReader     val;
        char              kbuf[32];
        char              vbuf[32];
        AxlJsonReader     obj;
        const char       *doc = "{\"o\":{\"k\x80\":\"v\"}}";

        test_check(axl_json_parse(doc, axl_strlen(doc),
                                        AXL_JSON_UTF8_REPAIR, &r)
                   && axl_json_get_object(&r, "o", &obj),
                   "utf8 repair: document with a raw bad byte in the KEY "
                   "parses");
        test_check(axl_json_object_begin(&r, "o", &it)
                   && axl_json_object_next(&it, kbuf, sizeof(kbuf), &val)
                   && axl_strcmp(kbuf, "k\xEF\xBF\xBD") == 0,
                   "utf8 repair: iteration reports the REPAIRED key name");
        test_check(axl_json_get_string(&obj, kbuf, vbuf, sizeof(vbuf))
                   && axl_strcmp(vbuf, "v") == 0,
                   "utf8 repair: and that exact name feeds back into a by-key "
                   "lookup — the two agree on what the key IS");
        axl_json_free(&r);
    }

    // --- REJECT_DUPLICATES names keys the same way the accessors do ---------
    // Two keys that differ only in bytes REPAIR collapses. Under REPAIR they
    // name the same thing, so they are duplicates — and the answer must not
    // depend on whether the object crossed DUP_LINEAR_MAX and switched from
    // the linear path to the hash one.
    {
        AxlJsonReader r;
        bool          both_reject = true;
        bool          both_accept = true;
        size_t        pad;

        for (pad = 0; pad <= 12; pad++) {
            AXL_AUTOPTR(AxlString) doc = axl_string_new(NULL);
            size_t                 i;
            char                   key[16];

            /* "\xC3" and "\xC4" are distinct raw bytes that BOTH repair to
               U+FFFD, so REPAIR must call them one key. `pad` walks the
               member count across the linear/hash threshold. */
            axl_string_append(doc, "{\"\xC3\":1,\"\xC4\":2");
            for (i = 0; i < pad; i++) {
                axl_snprintf(key, sizeof(key), ",\"p%02zu\":0", i);
                axl_string_append(doc, key);
            }
            axl_string_append(doc, "}");

            if (axl_json_parse(axl_string_str(doc),
                                     axl_strlen(axl_string_str(doc)),
                                     AXL_JSON_UTF8_REPAIR
                                     | AXL_JSON_REJECT_DUPLICATES, &r)) {
                both_reject = false;
            }
            axl_json_free(&r);

            /* Under RAW they are two different keys, at every width. */
            if (!axl_json_parse(axl_string_str(doc),
                                      axl_strlen(axl_string_str(doc)),
                                      AXL_JSON_UTF8_RAW
                                      | AXL_JSON_REJECT_DUPLICATES, &r)) {
                both_accept = false;
            }
            axl_json_free(&r);
        }
        test_check(both_reject,
                   "utf8 repair: two keys that REPAIR collapses are "
                   "duplicates "
                   "at every width — linear path and hash path agree");
        test_check(both_accept,
                   "utf8 repair: and under RAW they stay two distinct keys, "
                   "also at every width");
    }

    // --- an ill-formed KEY does not fabricate an out-of-memory --------------
    // The replacement is 3 bytes where the source byte was 1, so the key
    // decode buffer's bound has to allow 3x. It allowed 3/2 — correct until
    // REPAIR existed — and the duplicate check then refused valid documents
    // with AXL_JSON_ERR_NO_MEMORY.
    {
        AxlJsonReader r;
        const char   *doc = "{\"\x80\x80\\t\":1,\"b\":2}";

        test_check(axl_json_parse(doc, axl_strlen(doc),
                                        AXL_JSON_UTF8_REPAIR
                                        | AXL_JSON_REJECT_DUPLICATES, &r),
                   "utf8 repair: a key of ill-formed bytes does not overflow "
                   "the decode bound and report a false NO_MEMORY");
        axl_json_free(&r);
    }

    // --- the repair's own bound: growth that will not fit ------------------
    // The one branch here that can corrupt memory if it is wrong, and the one
    // no other row reaches — every buffer above is far larger than its input.
    // Sweeping small sizes walks the refusal boundary instead of guessing it.
    {
        const char *doc = "{\"a\":\"\x80\x80\"}";
        size_t      size;
        bool        all_sane = true;

        for (size = 1; size <= 10; size++) {
            AxlJsonReader r;
            char          buf[16];
            size_t        k;

            axl_memset(buf, (char)0xAA, sizeof(buf));
            buf[0] = '\0';   /* a refused call may not write; measure that */
            if (!axl_json_parse(doc, axl_strlen(doc),
                                      AXL_JSON_UTF8_REPAIR, &r)) {
                all_sane = false;
                break;
            }
            (void)axl_json_get_string(&r, "a", buf, size);
            axl_json_free(&r);

            /* Whatever fit, it must be NUL-terminated inside the buffer, hold
               only whole replacements, and never have touched a byte past
               the size it was given. */
            if (axl_strlen(buf) >= size) {
                all_sane = false;
            }
            for (k = size > 0 ? size : 1; k < sizeof(buf); k++) {
                if (buf[k] != (char)0xAA) {
                    all_sane = false;
                }
            }
            /* 3 = the byte length of U+FFFD; spelled out because the
               internal constant is not a public header. */
            if (axl_strlen(buf) % 3 != 0) {
                all_sane = false;   /* a replacement was split */
            }
        }
        test_check(all_sane,
                   "utf8 repair: at buffer sizes 1..10 the result is always "
                   "NUL-terminated, whole-replacement only, and writes "
                   "nothing "
                   "past the size given");
    }

    // --- the old block, kept: REPAIR through a sub-reader and iterator -----
    {
        AxlJsonReader     r;
        AxlJsonReader     sub;
        AxlJsonObjectIter it;
        AxlJsonReader     val;
        char              kbuf[32];
        char              vbuf[32];
        const char       *doc = "{\"o\":{\"a\":\"x\x80y\"}}";

        test_check(axl_json_parse(doc, axl_strlen(doc),
                                        AXL_JSON_UTF8_REPAIR, &r),
                   "utf8 repair: nested document parses");
        test_check(axl_json_get_object(&r, "o", &sub)
                   && axl_json_get_string(&sub, "a", vbuf, sizeof(vbuf))
                   && axl_strcmp(vbuf, "x\xEF\xBF\xBDy") == 0,
                   "utf8 repair: a SUB-READER repairs too — the mode is "
                   "inherited, not left at the default");
        axl_json_free(&r);

        doc = "{\"o\":{\"k\x80\":\"v\x80\"}}";
        test_check(axl_json_parse(doc, axl_strlen(doc),
                                        AXL_JSON_UTF8_REPAIR, &r),
                   "utf8 repair: document with a bad byte in the KEY parses");
        test_check(axl_json_object_begin(&r, "o", &it)
                   && axl_json_object_next(&it, kbuf, sizeof(kbuf), &val)
                   && axl_strcmp(kbuf, "k\xEF\xBF\xBD") == 0
                   && axl_json_value_string(&val, vbuf, sizeof(vbuf))
                   && axl_strcmp(vbuf, "v\xEF\xBF\xBD") == 0,
                   "utf8 repair: the object ITERATOR repairs both key and "
                   "value");
        axl_json_free(&r);
    }
}

// ---------------------------------------------------------------------------
// AXL_JSON_UTF8_STRICT on READ (P7) — parse-time, raw bytes only
// ---------------------------------------------------------------------------

static void
test_json_utf8_strict_read(void)
{
    const AxlJsonFlags STRICT_UTF8 = AXL_JSON_STRICT | AXL_JSON_UTF8_STRICT;

    // --- the same document under each mode ---------------------------------
    // One input, three outcomes, each pinned exactly. `\x80` is a bare
    // continuation byte: no lead, so it cannot begin any sequence.
    {
        const char   *doc = "{\"k\":\"a\x80z\"}";
        AxlJsonReader r;
        char          buf[16];

        test_check(axl_json_parse(doc, axl_strlen(doc),
                                        AXL_JSON_STRICT, &r),
                   "utf8 read: REPAIR (the zero value) still PARSES an "
                   "ill-formed document — it is not a validity mode");
        axl_json_free(&r);

        test_check(axl_json_parse(doc, axl_strlen(doc),
                                        AXL_JSON_STRICT | AXL_JSON_UTF8_RAW,
                                        &r)
                   && axl_json_get_string(&r, "k", buf, sizeof(buf))
                   && axl_strcmp(buf, "a\x80z") == 0,
                   "utf8 read: RAW parses and hands the bad byte back "
                   "verbatim");
        axl_json_free(&r);

        {
            const AxlJsonError *e;

            test_check(!axl_json_parse(doc, axl_strlen(doc),
                                             STRICT_UTF8, &r),
                       "utf8 read: STRICT REFUSES the document");
            e = axl_json_reader_error(&r);
            test_check(e->code == AXL_JSON_ERR_BAD_UTF8 && e->offset == 7,
                       "utf8 read: STRICT reports BAD_UTF8 at the first bad "
                       "BYTE, not at the string or the document");
            axl_json_free(&r);
        }
    }

    // --- position is a real position ----------------------------------------
    // Line and column, not just an offset — the whole argument for settling
    // this at parse time rather than in an accessor that can only say `false`.
    {
        const char         *doc = "{\n  \"a\": 1,\n  \"b\": \"\xC3\x28\"\n}";
        AxlJsonReader       r;
        const AxlJsonError *e;

        test_check(!axl_json_parse(doc, axl_strlen(doc),
                                         STRICT_UTF8, &r),
                   "utf8 read: a truncated 2-byte sequence is refused");
        e = axl_json_reader_error(&r);
        test_check(e->code == AXL_JSON_ERR_BAD_UTF8
                   && e->line == 3 && e->column == 9,
                   "utf8 read: with line and column, not merely an offset");
        axl_json_free(&r);
    }

    // --- KEYS are checked too, not only values ------------------------------
    {
        const char   *doc = "{\"k\xE2\x80\":1}";
        AxlJsonReader r;

        test_check(!axl_json_parse(doc, axl_strlen(doc), STRICT_UTF8, &r)
                   && axl_json_reader_error(&r)->code
                      == AXL_JSON_ERR_BAD_UTF8,
                   "utf8 read: an ill-formed KEY is refused as readily as a "
                   "value");
        axl_json_free(&r);
    }

    // --- what STRICT must NOT reject ----------------------------------------
    // It is an encoding check on the bytes present, not a code-point policy.
    {
        AxlJsonReader r;
        char          buf[16];

        test_check(axl_json_parse("{\"k\":\"\\ud800\"}", 14,
                                        STRICT_UTF8, &r),
                   "utf8 read: a lone surrogate as an ESCAPE is well-formed "
                   "JSON syntax and is NOT refused");
        test_check(axl_json_get_string(&r, "k", buf, sizeof(buf))
                   && axl_strcmp(buf, "\xEF\xBF\xBD") == 0,
                   "utf8 read: it still decodes to U+FFFD, as under any mode");
        axl_json_free(&r);

        /* 2-, 3- AND 4-byte. The 3-byte case is not decoration: a sabotage
           that broke the 3-byte ACCEPT branch left every other assertion in
           this function green, because no other document here feeds the
           checker a well-formed 3-byte sequence. */
        test_check(axl_json_parse(
                       "{\"k\":\"caf\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x98\x80\"}",
                       22, STRICT_UTF8, &r)
                   && axl_json_get_string(&r, "k", buf, sizeof(buf))
                   && axl_strcmp(buf,
                                 "caf\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x98\x80") == 0,
                   "utf8 read: well-formed 2-, 3- and 4-byte sequences pass "
                   "untouched");
        axl_json_free(&r);

        /* The exact ceiling. U+10FFFF is the last legal code point and its
           successor differs by one byte, so this pair pins the bound itself
           rather than the lead-byte range that catches \xF5 and above. */
        test_check(axl_json_parse("{\"k\":\"\xF4\x8F\xBF\xBF\"}", 12,
                                        STRICT_UTF8, &r),
                   "utf8 read: U+10FFFF, the highest legal code point, is "
                   "accepted");
        axl_json_free(&r);
        test_check(!axl_json_parse("{\"k\":\"\xF4\x90\x80\x80\"}", 12,
                                         STRICT_UTF8, &r)
                   && axl_json_reader_error(&r)->code == AXL_JSON_ERR_BAD_UTF8,
                   "utf8 read: and one code point past it is not");
        axl_json_free(&r);
    }

    // --- the ill-formed shapes, swept --------------------------------------
    // One hand-picked bad byte proves one branch of the lead-byte ladder.
    // Each row is a DIFFERENT way to be ill-formed, so a validator that
    // handled only orphan continuations would pass the first and fail here.
    {
        struct { const char *bytes; const char *msg; } row[] = {
            { "\x80",         "orphan continuation byte" },
            { "\xC3",         "2-byte lead with nothing after it" },
            { "\xE2\x80",     "3-byte lead, one continuation short" },
            { "\xF0\x9F\x98", "4-byte lead, one continuation short" },
            { "\xC0\xAF",     "2-byte overlong encoding of '/'" },
            { "\xE0\x80\xAF", "3-byte overlong — a different branch" },
            { "\xF0\x80\x80\xAF", "4-byte overlong — a third branch" },
            { "\xED\xA0\x80", "a surrogate encoded as raw bytes" },
            { "\xF5\x80\x80\x80", "beyond U+10FFFF" },
            { "\xFE",         "a byte no UTF-8 sequence may contain" },
        };
        size_t i;
        bool   all_refused = true;
        bool   all_parse   = true;
        char   refused_msg[128];

        axl_snprintf(refused_msg, sizeof(refused_msg),
                     "utf8 read: STRICT refuses every ill-formed shape — "
                     "orphan, truncated, overlong, surrogate, out-of-range, "
                     "impossible");

        for (i = 0; i < sizeof(row) / sizeof(row[0]); i++) {
            AxlJsonReader r;
            char          doc[32];

            axl_snprintf(doc, sizeof(doc), "{\"k\":\"%s\"}", row[i].bytes);
            if (axl_json_parse(doc, axl_strlen(doc), STRICT_UTF8, &r)
                || axl_json_reader_error(&r)->code != AXL_JSON_ERR_BAD_UTF8) {
                /* Name the ROW in the assertion text. An aggregate that
                   says only "one of these eight" sends the next reader back
                   to count them by hand, and the identifier is right here. */
                if (all_refused) {
                    axl_snprintf(refused_msg, sizeof(refused_msg),
                                 "utf8 read: STRICT did NOT refuse: %s",
                                 row[i].msg);
                }
                all_refused = false;
            }
            axl_json_free(&r);

            /* And every one of them must still PARSE without STRICT, or the
               check has leaked into the grammar. */
            if (!axl_json_parse(doc, axl_strlen(doc), AXL_JSON_STRICT,
                                      &r)) {
                all_parse = false;
            }
            axl_json_free(&r);
        }
        test_check(all_refused, refused_msg);
        test_check(all_parse,
                   "utf8 read: and WITHOUT it every one of them still "
                   "parses — the check did not leak into the grammar");
    }

    // --- a JSON5 COMMENT body is document bytes too -------------------------
    // The reason this validates the whole document rather than its string
    // tokens: a comment is the one other place arbitrary bytes survive
    // lexing, skip_ws walks it looking only for the terminator, and the
    // WRITER already repairs comment bodies. A token-only scan let these
    // through, so the two sides disagreed about the same document.
    {
        AxlJsonReader      r;
        const AxlJsonFlags j5 = AXL_JSON_JSON5 | AXL_JSON_UTF8_STRICT;

        const char *blk = "{\"k\":1} /* \xC3 */";

        test_check(!axl_json_parse(blk, axl_strlen(blk), j5, &r)
                   && axl_json_reader_error(&r)->code == AXL_JSON_ERR_BAD_UTF8,
                   "utf8 read: an ill-formed byte in a BLOCK comment is "
                   "refused");
        axl_json_free(&r);
        test_check(!axl_json_parse("// \x80\n{\"k\":1}", 12, j5, &r)
                   && axl_json_reader_error(&r)->code == AXL_JSON_ERR_BAD_UTF8,
                   "utf8 read: and in a LINE comment, before the document "
                   "even opens");
        axl_json_free(&r);
    }

    // --- the reserved mode value is refused, not silently ignored -----------
    // AXL_JSON_RELAXED already names UTF8_RAW, so `RELAXED | UTF8_STRICT` ORs
    // to the reserved value 3. Left undefined that reads as "not STRICT" and
    // hands back an unvalidated parse from a flags word that asked for
    // validation — the feature disabled by the act of requesting it.
    {
        AxlJsonReader r;

        test_check(!axl_json_parse("{\"k\":\"a\x80z\"}", 12,
                                         AXL_JSON_RELAXED
                                         | AXL_JSON_UTF8_STRICT, &r)
                   && axl_json_reader_error(&r)->code
                      == AXL_JSON_ERR_INVALID_ARGUMENT,
                   "utf8 read: RELAXED | UTF8_STRICT is the RESERVED field "
                   "value and is refused, never silently un-checked");
        axl_json_free(&r);
    }

    // --- UTF-8 is checked BEFORE duplicate keys -----------------------------
    // Pinned because it is a documented ordering, and because an ill-formed
    // byte makes a key's decoded name meaningless — reporting a duplicate
    // derived from one would be a worse answer, not merely a different one.
    {
        AxlJsonReader r;

        const char *both = "{\"a\":1,\"a\":2,\"z\":\"\x80\"}";

        test_check(!axl_json_parse(both, axl_strlen(both),
                                         STRICT_UTF8
                                         | AXL_JSON_REJECT_DUPLICATES, &r)
                   && axl_json_reader_error(&r)->code == AXL_JSON_ERR_BAD_UTF8,
                   "utf8 read: with BOTH checks on, the encoding error wins "
                   "even though the duplicate comes first in the document");
        axl_json_free(&r);
    }
}

// ---------------------------------------------------------------------------
// JSON Iterator Aliasing (P11)
// ---------------------------------------------------------------------------

static void
test_json_iter_aliasing(void)
{
    /* An iterator must not be retargeted by the caller REUSING the element
       reader it was built from. The iterator used to store a raw
       `const AxlJsonReader *` into the caller's struct and re-dereference it
       on every next(), so the sequence below walked element 1's tokens with
       element 0's indices — a silently wrong answer, with nothing freed and
       nothing to fault on, which is why ASan and valgrind both see a clean
       run. Found by design review, not by any test.

       String elements because a bare number in an array is not readable yet;
       the rest of P11 fixes that, and this has to land FIRST so
       AxlJsonObjectIter does not mirror the broken shape. */
    const char      *doc = "[[\"a\",\"b\"],[\"c\",\"d\"]]";
    AxlJsonReader    r, elem, sub;
    AxlJsonArrayIter outer, inner;
    char             buf[8];

    test_check(axl_json_parse(doc, axl_strlen(doc), AXL_JSON_RELAXED, &r)
               && axl_json_value_array_begin(&r, &outer)
               && axl_json_array_next(&outer, &elem),
               "iter alias: an array of arrays yields its first element");

    /* inner is built from elem while elem describes ["a","b"] ... */
    test_check(axl_json_value_array_begin(&elem, &inner),
               "iter alias: an inner iterator opens on that element");

    /* ... and now the caller reuses elem for the NEXT outer element, which is
       the ordinary way to walk an array and must not disturb `inner`.

       Asserting that elem really was RETARGETED, not merely that next()
       returned true: the retargeting IS the condition the assertions below
       test against, so a regression that returned true without writing
       `element` would leave elem describing ["a","b"], let every later
       assertion pass, and report green having never built the scenario. */
    {
        AxlJsonArrayIter check;
        AxlJsonReader    probe;
        char             cbuf[8];

        test_check(axl_json_array_next(&outer, &elem)
                   && axl_json_value_array_begin(&elem, &check)
                   && axl_json_array_next(&check, &probe)
                   && axl_json_value_string(&probe, cbuf, sizeof(cbuf))
                   && axl_strcmp(cbuf, "c") == 0,
                   "iter alias: the outer walk retargets the element reader "
                   "onto the second array");
    }

    test_check(axl_json_array_next(&inner, &sub)
               && axl_json_value_string(&sub, buf, sizeof(buf))
               && axl_strcmp(buf, "a") == 0,
               "iter alias: the inner iterator still yields ITS array's first "
               "element, not the reused reader's");
    test_check(axl_json_array_next(&inner, &sub)
               && axl_json_value_string(&sub, buf, sizeof(buf))
               && axl_strcmp(buf, "b") == 0,
               "iter alias: and its second, so the whole inner walk survives "
               "the reuse");
    test_check(!axl_json_array_next(&inner, &sub),
               "iter alias: the inner iterator stops after its own two "
               "elements");

    axl_json_free(&r);

    /* The same, entered by KEY. Every production call site in the tree uses
       axl_json_array_begin rather than the root form — and none of them can
       trigger the bug, because each finishes its inner loop before the outer
       next() reuses the element reader, which is exactly why this stayed
       latent long enough for a design review to find it rather than a user. */
    {
        const char      *keyed = "{\"a\":[[\"x\",\"y\"],[\"z\"]]}";
        AxlJsonReader    kr, kelem, ksub;
        AxlJsonArrayIter kouter, kinner;
        char             kbuf[8];

        test_check(axl_json_parse(keyed, axl_strlen(keyed), AXL_JSON_RELAXED, &kr)
                   && axl_json_array_begin(&kr, "a", &kouter)
                   && axl_json_array_next(&kouter, &kelem)
                   && axl_json_value_array_begin(&kelem, &kinner)
                   && axl_json_array_next(&kouter, &kelem)
                   && axl_json_array_next(&kinner, &ksub)
                   && axl_json_value_string(&ksub, kbuf, sizeof(kbuf))
                   && axl_strcmp(kbuf, "x") == 0,
                   "iter alias: the by-key entry point survives the reuse too");
        axl_json_free(&kr);
    }
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

/* Wrapper so axl_free (a macro) can be passed as an AxlDestroyNotify. */
static void
queue_free_data(void *p)
{
    axl_free(p);
}

/* Regression + contract for the stack/embedded teardown path. The trap the
   API-consistency audit found: axl_queue_free calls axl_free(struct), which
   corrupts a stack-initialized queue — so a stack queue must tear down with
   axl_queue_deinit. We can't call axl_queue_free(&stack) to prove corruption
   (UEFI has no memory-safety net — it would #GP), so we pin the SAFE path:
   deinit frees the nodes (no leak), leaves the struct reusable, and _full also
   frees element data. */
static void
test_queue_deinit(void)
{
    AxlMemStats before, after;

    /* deinit: frees all nodes, does NOT free the struct (reusable in place). */
    axl_mem_get_stats(&before);
    AxlQueue sq = AXL_QUEUE_INIT;
    axl_queue_push_tail(&sq, (void *)1);
    axl_queue_push_tail(&sq, (void *)2);
    axl_queue_push_tail(&sq, (void *)3);
    test_check(axl_queue_get_length(&sq) == 3, "queue deinit: 3 elems pushed");
    axl_queue_deinit(&sq);
    axl_mem_get_stats(&after);
    test_check(after.count == before.count, "queue deinit: frees all nodes (no leak)");
    test_check(axl_queue_is_empty(&sq) && axl_queue_get_length(&sq) == 0,
               "queue deinit: empty after deinit");
    test_check(axl_queue_push_tail(&sq, (void *)7) == AXL_OK,
               "queue deinit: struct reusable after deinit");
    test_check((intptr_t)axl_queue_pop_head(&sq) == 7,
               "queue deinit: reused queue works");
    axl_queue_deinit(&sq);

    axl_queue_deinit(NULL);   /* NULL-safe */

    /* deinit_full: frees element data too, struct still not freed. */
    axl_mem_get_stats(&before);
    AxlQueue dq = AXL_QUEUE_INIT;
    for (int i = 0; i < 3; i++) {
        void *p = axl_malloc(16);
        test_check(p != NULL, "queue deinit_full: data alloc");
        axl_queue_push_tail(&dq, p);
    }
    axl_queue_deinit_full(&dq, queue_free_data);
    axl_mem_get_stats(&after);
    test_check(after.count == before.count,
               "queue deinit_full: frees nodes AND element data (no leak)");
    test_check(axl_queue_is_empty(&dq), "queue deinit_full: empty after");

    axl_queue_deinit_full(NULL, queue_free_data);   /* NULL-safe */
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
    test_survived("cache: free(NULL) no crash");

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
pc_fill(size_t page_index, void *dst, size_t cap, void *user)
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
pc_owner_fill(size_t page_index, void *dst, size_t cap, void *user)
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
    test_check(axl_rb_tree_is_empty(&t), "rbtree: empty after init");
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
    test_check(axl_rb_tree_is_empty(&t), "rbtree: empty after erasing all");
    test_check(axl_rb_check_invariants(&t), "rbtree: empty tree passes invariants");
    test_check(axl_rb_black_height(&t) == 0, "rbtree: empty tree black height 0");

    /* EXACT, not a bound. The bound below is so slack that an off-by-one
       passed it unnoticed -- the function counted the NULL leaf and so
       returned bh+1, never 1, jumping 0 -> 2. A single black root is 1. */
    AxlRBTree t1;
    axl_rb_tree_init(&t1, NULL, NULL);
    rb_pool2[0].key = 42;
    rb_pool2[0].val = 42;
    rb_insert_key(&t1, &rb_pool2[0]);
    test_check(axl_rb_black_height(&t1) == 1,
               "rbtree: a single black root has black height exactly 1");
    test_check(axl_rb_check_invariants(&t1), "rbtree: one-node tree is valid");
    axl_rb_erase(&t1, &rb_pool2[0].node);
    test_check(axl_rb_black_height(&t1) == 0,
               "rbtree: back to 0 after erasing the only node");

    /* The DEPTH bound. rb_check recurses per level, and its job is to run on
       CORRUPT input -- where height is unbounded -- on a small UEFI stack. A
       hand-built degenerate left spine must be REFUSED, not recursed into
       until the stack gives out. */
    AxlRBTree deep;
    axl_rb_tree_init(&deep, NULL, NULL);
    deep.root = &rb_pool[0].node;
    rb_pool[0].node.parent = NULL;
    rb_pool[0].node.left = NULL;
    rb_pool[0].node.right = NULL;
    rb_pool[0].node.color = AXL_RB_BLACK;
    for (size_t d = 1; d < 300; d++) {
        rb_pool[d].node.parent = &rb_pool[d - 1].node;
        rb_pool[d].node.left   = NULL;
        rb_pool[d].node.right  = NULL;
        rb_pool[d].node.color  = AXL_RB_BLACK;
        rb_pool[d - 1].node.left = &rb_pool[d].node;
    }
    test_check(!axl_rb_check_invariants(&deep),
               "rbtree: a 300-deep spine is REFUSED, not recursed into");

    /* The public invariant checker. Every test above this point verifies the
       tree by IN-ORDER WALK, which proves it is sorted and says nothing about
       whether it is balanced -- a tree degenerated into a linked list answers
       every one of them correctly. Rebuild and check the structure itself,
       after every single mutation, so a rebalancing bug is localised to the
       operation that caused it. */
    AxlRBTree ti;
    axl_rb_tree_init(&ti, NULL, NULL);
    uint32_t ilcg = 987u;
    size_t   icount = 0;
    bool     all_ok = true;
    for (int attempts = 0; icount < 300 && attempts < 100000; attempts++) {
        ilcg = ilcg * 1103515245u + 12345u;
        int key = (int)((ilcg >> 8) % 100000u);
        bool dup = false;
        for (size_t j = 0; j < icount; j++) {
            if (rb_pool[j].key == key) { dup = true; break; }
        }
        if (dup) { continue; }
        rb_pool[icount].key = key;
        rb_pool[icount].val = (int)icount;
        rb_insert_key(&ti, &rb_pool[icount]);
        icount++;
        if (!axl_rb_check_invariants(&ti)) { all_ok = false; break; }
    }
    test_check(all_ok && icount == 300,
               "rbtree: invariants hold after every one of 300 inserts");

    /* Black height must stay within the red-black bound, 2*log2(n+1). For
       n=300 that is 16 -- a tree drifting toward degenerate breaks this long
       before it breaks an invariant. */
    int bh = axl_rb_black_height(&ti);
    test_check(bh > 0 && bh <= 16,
               "rbtree: black height within the 2*log2(n+1) bound");

    all_ok = true;
    for (size_t i = 0; i < 300; i += 3) {
        axl_rb_erase(&ti, &rb_pool[i].node);
        if (!axl_rb_check_invariants(&ti)) { all_ok = false; break; }
    }
    test_check(all_ok, "rbtree: invariants hold after every erase");

    /* POSITIVE CONTROL. A checker that always returned true would have passed
       everything above. Corrupt one colour and it must say so -- then put it
       back, because the tree is reused below. */
    AxlRBNode *victim = axl_rb_first(&ti);
    while (victim != NULL && victim->color != AXL_RB_BLACK) {
        victim = axl_rb_next(victim);
    }
    if (victim != NULL) {
        victim->color = AXL_RB_RED;
        test_check(!axl_rb_check_invariants(&ti),
                   "rbtree: checker DETECTS a corrupted colour");
        victim->color = AXL_RB_BLACK;
        test_check(axl_rb_check_invariants(&ti),
                   "rbtree: checker passes again once restored");
    } else {
        test_check(false, "rbtree: no black node found for the control");
    }

    /* A severed parent link is the other corruption class -- structure
       rather than colour. */
    AxlRBNode *child = ti.root->left != NULL ? ti.root->left : ti.root->right;
    if (child != NULL) {
        AxlRBNode *saved = child->parent;
        child->parent = NULL;
        test_check(!axl_rb_check_invariants(&ti),
                   "rbtree: checker DETECTS a severed parent link");
        child->parent = saved;
        test_check(axl_rb_check_invariants(&ti),
                   "rbtree: checker passes again once relinked");
    } else {
        test_check(false, "rbtree: root had no child for the control");
    }

    while ((it = axl_rb_first(&ti)) != NULL) {
        axl_rb_erase(&ti, it);
    }

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
    test_survived("radix: free(NULL) no crash");

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

// A NULL value is a VALUE, not an absence. The tree used to infer a key's
// presence from `node->value != NULL`, which made every one of the assertions
// below wrong: the entry was counted but unremovable, re-inserting it counted
// it again, and overwriting a live value with NULL stranded the key forever.
//
// Presence is now a flag on the node, so `size()` counts keys and `remove()`
// can reach every key `size()` counts. Lookup still cannot distinguish a
// stored NULL from an absent key — that is the return convention (`void *`,
// NULL for miss) and is documented, not a defect.
static void
test_radix_tree_null_value(void)
{
    AxlRadixTree *t = axl_radix_tree_new();

    // A fresh key with a NULL value: counted once, and REMOVABLE.
    test_check(axl_radix_tree_insert(t, "/a", NULL) == AXL_OK,
        "radix_null: insert of a NULL value succeeds");
    test_check(axl_radix_tree_size(t) == 1, "radix_null: counted once");
    test_check(axl_radix_tree_remove(t, "/a"),
        "radix_null: a NULL-valued key can be removed");
    test_check(axl_radix_tree_size(t) == 0, "radix_null: size back to 0");

    // Re-inserting the SAME key must not count it twice. This is the exact
    // double-count: both inserts took the `else { tree->size++; }` branch
    // because `node->value` was NULL each time.
    axl_radix_tree_insert(t, "/b", NULL);
    axl_radix_tree_insert(t, "/b", NULL);
    test_check(axl_radix_tree_size(t) == 1,
        "radix_null: inserting the same key twice counts once");
    test_check(axl_radix_tree_remove(t, "/b"), "radix_null: still removable");
    test_check(axl_radix_tree_size(t) == 0, "radix_null: size 0 after remove");

    // Overwriting a LIVE value with NULL. The key stays present and stays
    // removable; previously it became permanently unreachable while still
    // being counted.
    axl_radix_tree_insert(t, "/c", (void *)0x1234);
    axl_radix_tree_insert(t, "/c", NULL);
    test_check(axl_radix_tree_size(t) == 1, "radix_null: overwrite keeps size 1");
    test_check(axl_radix_tree_lookup(t, "/c") == NULL,
        "radix_null: overwritten value reads back NULL");
    test_check(axl_radix_tree_remove(t, "/c"),
        "radix_null: a key overwritten with NULL is still removable");
    test_check(axl_radix_tree_size(t) == 0, "radix_null: size 0 again");

    // And the reverse: NULL then a real value.
    axl_radix_tree_insert(t, "/d", NULL);
    axl_radix_tree_insert(t, "/d", (void *)0x5678);
    test_check(axl_radix_tree_size(t) == 1, "radix_null: NULL-then-value counts once");
    test_check(axl_radix_tree_lookup(t, "/d") == (void *)0x5678,
        "radix_null: the real value is readable");

    // foreach must VISIT a NULL-valued entry: it is an entry, and a visit
    // count that disagrees with size() is how the accounting bug would come
    // back. Counting only visits would miss it, so the count is compared
    // against size().
    axl_radix_tree_insert(t, "/e", NULL);
    radix_foreach_count = 0;
    axl_radix_tree_foreach(t, radix_foreach_counter, NULL);
    test_check(axl_radix_tree_size(t) == 2, "radix_null: two entries present");
    test_check(radix_foreach_count == axl_radix_tree_size(t),
        "radix_null: foreach visits every counted entry, NULL values included");

    axl_radix_tree_free(t);

    // An OWNING tree must not call the destructor for a NULL value, and must
    // still count the entry. A destructor invoked on NULL is the obvious way
    // to get this wrong in the other direction.
    radix_free_count = 0;
    AxlRadixTree *o = axl_radix_tree_new_full(radix_free_counter);
    axl_radix_tree_insert(o, "/x", NULL);
    axl_radix_tree_insert(o, "/y", (void *)1);
    test_check(axl_radix_tree_size(o) == 2, "radix_null: owning tree counts both");
    axl_radix_tree_remove(o, "/x");
    test_check(radix_free_count == 0,
        "radix_null: removing a NULL value does not call the destructor");
    axl_radix_tree_free(o);
    test_check(radix_free_count == 1,
        "radix_null: teardown frees only the non-NULL value");
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
    test_survived("ring: free(NULL) no crash");

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
    if (axl_compress(fmt, data, len, level, &comp, &comp_len) != AXL_OK) {
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
        int rc = axl_compress(AXL_COMPRESS_LZMA, "", 0,
                              AXL_COMPRESS_LEVEL_DEFAULT, &c, &cl);
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
        int rc = axl_compress(AXL_COMPRESS_LZMA, "a", 1,
                              AXL_COMPRESS_LEVEL_DEFAULT, &c, &cl);
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
            int rc = axl_compress(AXL_COMPRESS_LZMA, in, len,
                                  AXL_COMPRESS_LEVEL_DEFAULT, &c, &cl);
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
    if (axl_compress(AXL_COMPRESS_GZIP, data, len,
                     AXL_COMPRESS_LEVEL_DEFAULT, &gz, &gz_len) != AXL_OK) {
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
    test_check(axl_compress(AXL_COMPRESS_GZIP, "x", 1,
                            AXL_COMPRESS_LEVEL_DEFAULT, NULL, &out_len) == AXL_ERR,
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

    /* ... and on no stream at all. The NULL guard used to be written out in
       axl_compress_writer_finish; it now rides on axl_stream_ctx refusing a
       NULL stream, so it is worth an assertion of its own rather than trust
       in a call one layer down. */
    test_check(axl_compress_writer_finish(NULL) == AXL_ERR,
               "compress writer: finish on NULL rejected");

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

/* The compressing writer is built through the PUBLIC axl_stream_open_custom,
   so its stream owns a heap COPY of the label "compress" rather than pointing
   at a literal, and axl_fclose has to release that as well as the context. No
   other assertion in this file can see the difference -- the round-trips above
   pass identically whether or not the label leaks -- so it needs its own.

   The sink is inside the measured window on purpose: close finalizes, which
   compresses into the sink, so the sink's own growth is part of what has to
   come back. Everything opened here is closed here, so the count must land
   exactly on its baseline. */
static void
test_compress_writer_ownership(void)
{
    const char  *data = "ownership payload ownership payload ownership payl";
    AxlMemStats  before, after;
    AxlStream   *sink;
    AxlStream   *w;

    /* Warm the codec's first-use state so the baseline is steady state. */
    sink = axl_bufopen();
    w    = axl_gzip_writer(sink, AXL_COMPRESS_LEVEL_DEFAULT);
    axl_write(w, data, 50);
    axl_fclose(w);
    axl_fclose(sink);

    axl_mem_get_stats(&before);
    sink = axl_bufopen();
    w    = axl_gzip_writer(sink, AXL_COMPRESS_LEVEL_DEFAULT);
    test_check(w != NULL, "compown: open a compressing writer");
    if (w != NULL) {
        axl_write(w, data, 50);
        axl_fclose(w);
    }
    axl_fclose(sink);
    axl_mem_get_stats(&after);

    test_check(after.count == before.count,
               "compown: writer open+close returns the allocation count to baseline");
    test_check(after.bytes == before.bytes,
               "compown: ... and the allocated bytes with it");
}

/* The filter rule from axl-stream.h, on the compressing pair: a peer that
   transcodes rewrites DEFLATE bytes code point by code point, so the filter
   refuses one rather than producing an archive nothing can read while every
   call reports success. The writer checks twice -- at construction, and again
   when it finalizes, because that is when the sink is actually written. */
static void
test_compress_filters_refuse_a_transcoding_peer(void)
{
    const char *data = "payload for the transcoding-peer checks";
    size_t      len  = 39;
    AxlStream  *sink;
    AxlStream  *src;
    AxlStream  *w;
    size_t      n;

    /* 1) A writer over a sink that transcodes is refused at construction,
          and the refusal is inert -- the sink keeps its setting and is
          still usable. */
    sink = axl_bufopen();
    axl_stream_set_encoding(sink, AXL_ENC_UCS2_LE);
    test_check(axl_gzip_writer(sink, AXL_COMPRESS_LEVEL_DEFAULT) == NULL,
               "compfilter: a writer over a transcoding sink is refused");
    test_check(axl_stream_get_encoding(sink) == AXL_ENC_UCS2_LE,
               "compfilter: and the refusal leaves the sink's encoding alone");
    axl_fclose(sink);

    /* 2) A reader over a source that transcodes is refused on the same
          terms -- the mirror, and the one that would otherwise report a
          decode error that says nothing about the real cause. The source
          holds a VALID gzip member, so a NULL here can only be the refusal:
          feeding junk would have returned NULL for the ordinary decode
          reason and proved nothing. */
    {
        void   *gz  = NULL;
        size_t  gzn = 0;
        AxlStream *r;

        test_check(axl_compress(AXL_COMPRESS_GZIP, data, len,
                                AXL_COMPRESS_LEVEL_DEFAULT, &gz, &gzn) == AXL_OK,
                   "compfilter: fixture -- a real gzip member to feed it");
        src = axl_bufopen();
        axl_write(src, gz, gzn);
        axl_free(gz);
        axl_fseek(src, 0, AXL_SEEK_SET);

        axl_stream_set_encoding(src, AXL_ENC_UCS2_LE);
        test_check(axl_gzip_reader(src) == NULL,
                   "compfilter: a reader over a transcoding source is refused");
        /* A NULL alone does not distinguish "refused" from "drained the
           source, transcoded it into rubble and failed to decode it" -- the
           unfixed path returns NULL too. The source's POSITION does: a
           refusal never reads it. */
        test_check(axl_ftell(src) == 0 && axl_feof(src) == false,
                   "compfilter: and it did not read the source to find out");

        /* Same bytes, same position, encoding cleared -- it decodes. That is
           what makes the line above measure the refusal and not the data. */
        axl_stream_set_encoding(src, AXL_ENC_UTF8);
        axl_fseek(src, 0, AXL_SEEK_SET);
        r = axl_gzip_reader(src);
        test_check(r != NULL,
                   "compfilter: and the same source undecoded reads fine");
        axl_fclose(r);
        axl_fclose(src);
    }

    /* 3) The sink can acquire an encoding AFTER the writer was built, and
          finalize is the moment the bytes actually move -- so it checks
          again, and refuses rather than emitting a corrupt member. */
    sink = axl_bufopen();
    w    = axl_gzip_writer(sink, AXL_COMPRESS_LEVEL_DEFAULT);
    test_check(w != NULL, "compfilter: a writer over a clean sink is built");
    if (w != NULL) {
        axl_write(w, data, len);
        axl_stream_set_encoding(sink, AXL_ENC_UCS2_LE);
        test_check(axl_compress_writer_finish(w) == AXL_ERR,
                   "compfilter: finish refuses a sink that gained an encoding");
        n = 0;
        axl_bufdata(sink, &n);
        test_check(n == 0, "compfilter: and nothing corrupt was written to it");
        axl_stream_set_encoding(sink, AXL_ENC_UTF8);
        axl_fclose(w);
    }
    axl_fclose(sink);
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
    axl_compress(AXL_COMPRESS_ZLIB, data, len,
                 AXL_COMPRESS_LEVEL_DEFAULT, &zl, &zln);
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

// ---------------------------------------------------------------------------
// axl_array_sized_new / axl_array_steal / element-cleanup hooks
//
// Closer alignment with GArray. steal has g_array_steal semantics — the array
// is EMPTIED, not destroyed — and out_len is an ELEMENT COUNT, which is where
// an element-vs-byte confusion would show up.
// ---------------------------------------------------------------------------

// Cleanup-hook probes. Counters are file-static so the callbacks can be plain
// functions matching AxlDestroyNotify.
static int  ck_calls;
static int  ck_sum;      // sum of the int VALUES the value-mode hook saw
static void *ck_last_ptr;

static void
ck_value_hook(void *slot)
{
    ck_calls++;
    ck_sum += *(int *)slot;   // slot points AT the element
}

static void
ck_ptr_hook(void *stored)
{
    ck_calls++;
    ck_last_ptr = stored;     // the STORED pointer, not the slot
}

static void
ck_reset(void)
{
    ck_calls = 0;
    ck_sum = 0;
    ck_last_ptr = NULL;
}

static void
test_array_sized_steal_clear(void)
{
    AxlArray *a;
    size_t    n;
    int       v;

    // --- axl_array_sized_new -------------------------------------------
    AxlMemStats m0, m1;
    size_t      sized_allocs, unsized_allocs;

    // Count ALLOCATIONS, not just correctness. Contents-survive assertions
    // pass just as well against a sized_new that ignores `reserved`, so they
    // pin nothing; the whole point of the hint is the regrow chain it removes.
    axl_mem_get_stats(&m0);
    a = axl_array_sized_new(sizeof(int), 1000);
    for (v = 0; v < 1000; v++) {
        axl_array_append(a, &v);
    }
    axl_mem_get_stats(&m1);
    sized_allocs = m1.total_count - m0.total_count;
    test_check(a != NULL && axl_array_len(a) == 1000
               && *(int *)axl_array_get(a, 0) == 0
               && *(int *)axl_array_get(a, 999) == 999,
               "array sized_new: 1000 appends land in order");
    axl_array_free(a);

    axl_mem_get_stats(&m0);
    a = axl_array_new(sizeof(int));
    for (v = 0; v < 1000; v++) {
        axl_array_append(a, &v);
    }
    axl_mem_get_stats(&m1);
    unsized_allocs = m1.total_count - m0.total_count;
    axl_array_free(a);

    // Exactly two: the struct and one buffer. No grow-and-copy at all.
    test_check(sized_allocs == 2,
               "array sized_new: 1000 appends cost 2 allocations, zero regrows");
    test_check(unsized_allocs > sized_allocs,
               "array sized_new: the unsized array really does regrow (control)");

    a = axl_array_sized_new(sizeof(int), 1000);
    test_check(a != NULL && axl_array_len(a) == 0,
               "array sized_new: reserves capacity but starts EMPTY");
    axl_array_free(a);

    // `reserved` is caller-controlled, so reserved * element_size must not be
    // allowed to wrap. It used to: axl_calloc(1, cap * es) hid the product
    // from axl_calloc's own guard, so the array kept the huge capacity while
    // holding a tiny buffer and the FIRST append wrote past it.
    test_check(axl_array_sized_new(16, (SIZE_MAX / 16) + 1) == NULL,
               "array sized_new: a reserved*element_size overflow is refused");
    test_check(axl_array_sized_new(2, SIZE_MAX) == NULL,
               "array sized_new: SIZE_MAX elements is refused, not wrapped");

    a = axl_array_sized_new(sizeof(int), 0);
    v = 7;
    test_check(a != NULL && axl_array_append(a, &v) == AXL_OK
               && axl_array_len(a) == 1,
               "array sized_new: reserved 0 behaves like axl_array_new");
    axl_array_free(a);
    test_check(axl_array_sized_new(0, 16) == NULL,
               "array sized_new: element_size 0 is rejected");

    // --- axl_array_steal -----------------------------------------------
    // Element size deliberately > 1 so out_len being a BYTE count would show.
    a = axl_array_new(sizeof(int64_t));
    int64_t w;
    w = 10; axl_array_append(a, &w);
    w = 20; axl_array_append(a, &w);
    w = 30; axl_array_append(a, &w);

    n = 999;
    int64_t *stolen = axl_array_steal(a, &n);
    test_check(stolen != NULL && n == 3,
               "array steal: out_len is the ELEMENT count, not bytes (3, not 24)");
    test_check(stolen != NULL && stolen[0] == 10 && stolen[1] == 20 && stolen[2] == 30,
               "array steal: the handed-back block holds the elements");
    test_check(axl_array_len(a) == 0,
               "array steal: the array is left EMPTY");

    // g_array_steal, not g_array_free(arr, FALSE): still valid, still usable.
    w = 40;
    test_check(axl_array_append(a, &w) == AXL_OK
               && axl_array_len(a) == 1
               && *(int64_t *)axl_array_get(a, 0) == 40,
               "array steal: the array stays VALID and reusable afterwards");
    // The stolen block is independent of the array's new buffer.
    test_check(stolen[0] == 10,
               "array steal: the stolen block is unaffected by later appends");
    axl_free(stolen);

    // Steal twice in a row — the second must not hand back the first block.
    void *s1 = axl_array_steal(a, &n);
    test_check(n == 1, "array steal: second steal reports the new length");
    n = 999;
    void *s2 = axl_array_steal(a, &n);
    test_check(n == 0, "array steal: stealing an already-emptied array gives len 0");
    test_check(s2 == NULL,
               "array steal: a second steal hands back NULL, never the first block");
    axl_free(s1);
    axl_free(s2);
    axl_array_free(a);

    // Steal from a freshly-created array. Note this is NOT the buffer-less
    // path — construction always allocates — it is the length-0 path.
    a = axl_array_new(sizeof(int));
    n = 999;
    void *empty = axl_array_steal(a, &n);
    test_check(n == 0, "array steal: empty array reports 0 elements");
    axl_free(empty);
    test_check(axl_array_steal(a, NULL) == NULL,
               "array steal: a NULL out_len is accepted (already-stolen gives NULL)");
    axl_array_free(a);
    test_check(axl_array_steal(NULL, &n) == NULL,
               "array steal: NULL array yields NULL");

    // --- value-mode clear hook, on EVERY discarding path ----------------
    // free
    ck_reset();
    a = axl_array_new(sizeof(int));
    v = 1; axl_array_append(a, &v);
    v = 2; axl_array_append(a, &v);
    axl_array_set_clear_func(a, ck_value_hook);
    axl_array_free(a);
    test_check(ck_calls == 2 && ck_sum == 3, "array clear_func: runs on free");

    // clear
    ck_reset();
    a = axl_array_new(sizeof(int));
    axl_array_set_clear_func(a, ck_value_hook);
    v = 4; axl_array_append(a, &v);
    v = 5; axl_array_append(a, &v);
    axl_array_clear(a);
    test_check(ck_calls == 2 && ck_sum == 9 && axl_array_len(a) == 0,
               "array clear_func: runs on clear");

    // remove_index
    ck_reset();
    v = 6; axl_array_append(a, &v);
    v = 7; axl_array_append(a, &v);
    axl_array_remove_index(a, 0);
    test_check(ck_calls == 1 && ck_sum == 6,
               "array clear_func: runs on remove_index, for the removed element");

    // remove_index_fast
    ck_reset();
    v = 8; axl_array_append(a, &v);          // [7,8]
    axl_array_remove_index_fast(a, 0);       // discards 7
    test_check(ck_calls == 1 && ck_sum == 7,
               "array clear_func: runs on remove_index_fast");

    // remove_range
    ck_reset();
    axl_array_clear(a);
    ck_reset();
    v = 1; axl_array_append(a, &v);
    v = 2; axl_array_append(a, &v);
    v = 3; axl_array_append(a, &v);
    axl_array_remove_range(a, 0, 2);
    test_check(ck_calls == 2 && ck_sum == 3,
               "array clear_func: runs on remove_range, once per element");

    // shrinking set_size discards; growing set_size must NOT
    ck_reset();
    axl_array_clear(a);
    ck_reset();
    v = 5; axl_array_append(a, &v);
    v = 6; axl_array_append(a, &v);
    axl_array_set_size(a, 1);
    test_check(ck_calls == 1 && ck_sum == 6,
               "array clear_func: runs on a SHRINKING set_size");
    ck_reset();
    axl_array_set_size(a, 8);
    test_check(ck_calls == 0, "array clear_func: does NOT run on a growing set_size");

    // steal transfers ownership, so it must NOT run the hook
    ck_reset();
    axl_array_set_size(a, 0);
    ck_reset();
    v = 9; axl_array_append(a, &v);
    void *taken = axl_array_steal(a, &n);
    test_check(ck_calls == 0 && n == 1,
               "array clear_func: does NOT run on steal (ownership transfers)");
    axl_free(taken);
    axl_array_free(a);

    // --- pointer-mode free hook -----------------------------------------
    // The hook must receive the STORED pointer, not the slot's address. If it
    // got the slot, passing axl_free would free into the array's own buffer.
    ck_reset();
    a = axl_array_new(sizeof(void *));
    test_check(axl_array_set_ptr_free_func(a, ck_ptr_hook) == AXL_OK,
               "array ptr_free_func: accepted on a pointer-mode array");
    char *owned = axl_malloc(8);
    axl_array_append_ptr(a, owned);
    void *slot_addr = axl_array_get(a, 0);
    axl_array_clear(a);
    test_check(ck_calls == 1 && ck_last_ptr == owned && ck_last_ptr != slot_addr,
               "array ptr_free_func: receives the STORED pointer, not the slot");
    axl_free(owned);
    axl_array_free(a);

    // Real ownership over REAL heap pointers, with the release actually
    // observed. An earlier draft of this block ran axl_free_impl and asserted
    // nothing at all — stubbing set_ptr_free_func to ignore its argument left
    // it green. Count the frees AND release for real, so both halves are
    // pinned: allocations outstanding must return to the pre-block level.
    // (a) The hook runs once per stored element. Sentinel pointers, never
    // dereferenced or freed, so this block owns nothing.
    ck_reset();
    a = axl_array_new(sizeof(void *));
    axl_array_set_ptr_free_func(a, ck_ptr_hook);
    axl_array_append_ptr(a, (void *)0x1000);
    axl_array_append_ptr(a, (void *)0x2000);
    axl_array_clear(a);
    test_check(ck_calls == 2,
               "array ptr_free_func: hook runs once per stored pointer");
    axl_array_free(a);

    // (b) Real ownership over REAL heap pointers, with the release OBSERVED.
    // An earlier draft ran axl_free_impl and asserted nothing — stubbing
    // set_ptr_free_func to ignore its argument left it green. Balancing the
    // live-allocation count is what makes the free itself load-bearing.
    AxlMemStats o0, o1;
    axl_mem_get_stats(&o0);
    a = axl_array_new(sizeof(void *));
    axl_array_set_ptr_free_func(a, axl_free_impl);
    axl_array_append_ptr(a, axl_malloc(16));
    axl_array_append_ptr(a, axl_malloc(16));
    axl_array_free(a);
    axl_mem_get_stats(&o1);
    test_check(o1.count == o0.count,
               "array ptr_free_func: axl_free_impl really releases the elements");

    // A value-mode array is REJECTED rather than reinterpreted.
    a = axl_array_new(sizeof(int));
    test_check(axl_array_set_ptr_free_func(a, axl_free_impl) == AXL_ERR,
               "array ptr_free_func: rejected when element_size != sizeof(void*)");
    test_check(axl_array_set_ptr_free_func(NULL, axl_free_impl) == AXL_ERR,
               "array ptr_free_func: NULL array is rejected");
    axl_array_free(a);

    // Setting either hook replaces the other; NULL clears.
    ck_reset();
    a = axl_array_new(sizeof(void *));
    axl_array_set_ptr_free_func(a, ck_ptr_hook);
    axl_array_set_clear_func(a, NULL);
    axl_array_append_ptr(a, (void *)0x1234);
    axl_array_clear(a);
    test_check(ck_calls == 0, "array hooks: setting NULL clears a previously set hook");
    axl_array_free(a);
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

// The contiguous-buffer accessors. Every assertion here is stated against
// axl_array_get()/axl_array_get_ptr(), which already work, rather than against
// a literal — the contract is "the same bytes, without the per-element call",
// so an agreement test is what actually pins it.
static void
test_array_data_and_element_size(void)
{
    // Deliberately not int: a stride the compiler would not have guessed, so
    // an implementation returning sizeof(int) or sizeof(void *) by accident
    // fails rather than coincidentally passing.
    struct Rec { int32_t a; int64_t b; char c; };

    test_check(axl_array_data(NULL) == NULL, "data: NULL array -> NULL");
    test_check(axl_array_element_size(NULL) == 0, "elemsize: NULL array -> 0");

    AxlArray *a = axl_array_new(sizeof(struct Rec));
    test_check(axl_array_element_size(a) == sizeof(struct Rec),
        "elemsize: reports the constructor's stride");
    test_check(axl_array_data(a) != NULL, "data: fresh array owns a buffer");

    for (int i = 0; i < 12; i++) {
        struct Rec r = { .a = i, .b = (int64_t)i * 1000, .c = (char)('a' + i) };
        axl_array_append(a, &r);
    }

    // The base pointer IS element 0, and the stride between elements is
    // exactly element_size. These two together are the whole addressing
    // contract a typed reader casts on.
    test_check(axl_array_data(a) == axl_array_get(a, 0),
        "data: base pointer is element 0");
    test_check((size_t)((uint8_t *)axl_array_get(a, 1)
                        - (uint8_t *)axl_array_data(a))
               == axl_array_element_size(a),
        "data: element 1 sits exactly element_size past the base");

    // Reading the whole array through the base pointer agrees with reading it
    // one axl_array_get() at a time, field by field. Comparing only `a` would
    // pass for a stride that was wrong in a way the first field survives.
    const struct Rec *base = (const struct Rec *)axl_array_data(a);
    bool agrees = true;
    for (size_t i = 0; i < axl_array_len(a); i++) {
        const struct Rec *via_get = (const struct Rec *)axl_array_get(a, i);
        if (base[i].a != via_get->a || base[i].b != via_get->b
            || base[i].c != via_get->c) {
            agrees = false;
        }
    }
    test_check(agrees, "data: 12 elements read via base match axl_array_get");
    test_check(base[11].a == 11 && base[11].b == 11000 && base[11].c == 'l',
        "data: last element carries the values that were appended");

    // The documented invalidation rule, observed rather than asserted away:
    // growth past capacity moves the buffer. Recorded so a future change that
    // makes the buffer stable does not quietly leave the warning wrong.
    const void *before = axl_array_data(a);
    for (int i = 0; i < 200; i++) {
        struct Rec r = { .a = i, .b = 0, .c = 'x' };
        axl_array_append(a, &r);
    }
    test_check(axl_array_len(a) == 212, "data: 212 elements after growth");
    test_check(axl_array_data(a) != before,
        "data: growth past capacity moved the buffer (the invalidation rule)");
    test_check(axl_array_data(a) == axl_array_get(a, 0),
        "data: base still tracks element 0 after a realloc");
    axl_array_free(a);

    // Pointer mode: the buffer holds the stored pointers themselves.
    AxlArray *p = axl_array_new(sizeof(void *));
    test_check(axl_array_element_size(p) == sizeof(void *),
        "elemsize: pointer mode is sizeof(void *)");
    axl_array_append_ptr(p, (void *)0xA1);
    axl_array_append_ptr(p, (void *)0xB2);
    axl_array_append_ptr(p, (void *)0xC3);
    void *const *slots = (void *const *)axl_array_data(p);
    test_check(slots[0] == (void *)0xA1 && slots[1] == (void *)0xB2
               && slots[2] == (void *)0xC3,
        "data: pointer mode holds the stored pointers");
    test_check(slots[1] == axl_array_get_ptr(p, 1),
        "data: pointer-mode slot agrees with axl_array_get_ptr");
    axl_array_free(p);

    // After a steal the array owns no buffer. NULL must be paired with a
    // length of 0 — that pairing is what lets a (pointer, length) reader skip
    // a separate NULL test, so it is asserted rather than assumed.
    AxlArray *s = axl_array_new(sizeof(int));
    for (int i = 0; i < 4; i++) { axl_array_append(s, &i); }
    size_t stolen_len = 0;
    void *stolen = axl_array_steal(s, &stolen_len);
    test_check(stolen != NULL && stolen_len == 4, "data: steal handed over 4");
    test_check(axl_array_data(s) == NULL, "data: stolen array reports NULL");
    test_check(axl_array_len(s) == 0, "data: NULL base is paired with length 0");
    test_check(axl_array_element_size(s) == sizeof(int),
        "elemsize: survives a steal (the array stays usable)");
    axl_free(stolen);
    axl_array_free(s);
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

// ---------------------------------------------------------------------------
// Pull scanner (P12)
// ---------------------------------------------------------------------------

/* Render a whole scan as one line: `{@0/d0 KEY(a)@1/d1 ...`.
 *
 * A single exact-string assertion over the WHOLE event stream, rather than one
 * per event. Kind, text, offset and depth all have to be right simultaneously
 * or the line differs -- and per CLAUDE.md an exact whole-document assertion is
 * what a substring check would let slip. */
/* Append to @a out, refusing rather than overflowing.
 *
 * axl_snprintf returns the length it WOULD have written, so the natural
 * `n += axl_snprintf(...)` walks n past the buffer and hands the next call
 * `out + n` (out of bounds) with an underflowed size. It does not fire on
 * today's short fixtures, which is precisely why it would first surface in
 * UEFI, with no guard page, on some later longer one.
 *
 * @return false when the text did not fit, so a truncated render can never be
 *     compared as if it were the whole scan. */
static bool
scan_put(char *out, size_t size, size_t *n, const char *fmt, ...)
{
    va_list ap;
    int     w;

    if (*n >= size) {
        return false;
    }
    va_start(ap, fmt);
    w = axl_vsnprintf(out + *n, size - *n, fmt, ap);
    va_end(ap);
    if (w < 0 || (size_t)w >= size - *n) {
        return false;
    }
    *n += (size_t)w;
    return true;
}

/* Drain a scanner into @a out. Shared by the contiguous and the pull-mode
 * checks below, so a chunking differential compares two SCANS rather than two
 * renderers. */
static bool
scan_drain(AxlJsonScanner *s, char *out, size_t size)
{
    AxlJsonEvent ev;
    size_t       n = 0;

    out[0] = '\0';
    while (axl_json_scanner_next(s, &ev)) {
        static const char *kinds[] = { "EOF", "{", "}", "[", "]",
                                       "KEY", "STR", "NUM", "BOOL", "NUL" };

        if (!scan_put(out, size, &n, "%s", kinds[ev.kind])
            || (ev.text != NULL
                && !scan_put(out, size, &n, "(%.*s)", (int)ev.len, ev.text))
            || !scan_put(out, size, &n, "@%u/d%u ", (unsigned)ev.offset,
                         ev.depth)) {
            return false;
        }
    }
    /* The final verdict rides along, so a scan that STOPPED early cannot
       compare equal to one that finished.
     *
     * The POSITION rides along only on a failure, and that asymmetry is
     * deliberate rather than lazy: a successful scan leaves the record
     * zeroed, so appending "@0 0:0" to every passing row would be noise --
     * while on a failure the offset, line and column are the whole point, and
     * they are what the chunking differential below compares. A scan that
     * carried line and column wrongly across a refill differs HERE and
     * nowhere else. */
    {
        const AxlJsonError *e = axl_json_scanner_error(s);

        if (e->code == AXL_JSON_OK) {
            return scan_put(out, size, &n, "|e0");
        }
        /* missing_flag rides along too. Without it a reclassification that
           kept the CODE and dropped the remedy -- exactly what lex_fail's
           window rule does when it fires -- is invisible to this oracle and
           to the chunking differential built on it. */
        return scan_put(out, size, &n, "|e%d@%u %u:%u f%llx", (int)e->code,
                        (unsigned)e->offset, e->line, e->column,
                        (unsigned long long)e->missing_flag);
    }
}

static void
scan_check(const char *doc, AxlJsonFlags flags, const char *want,
           const char *msg)
{
    AxlJsonSource  src;
    AxlJsonScanner s;
    char           out[1024];

    axl_json_source_init_mem(&src, doc, axl_strlen(doc));
    if (!axl_json_scanner_init(&s, &src, flags)) {
        test_check(false, msg);
        return;
    }
    test_check(scan_drain(&s, out, sizeof(out))
                   && axl_strcmp(out, want) == 0, msg);
    axl_json_scanner_free(&s);
}

/* A pull source that hands back at most @c chunk bytes per call.
 *
 * @c calls is counted so a test can assert the scanner asked more than once
 * -- otherwise a "chunked" fixture whose chunk exceeded the document would
 * silently be testing the contiguous path under another name. */
typedef struct {
    const char *data;
    size_t      len;
    size_t      pos;
    size_t      chunk;
    unsigned    calls;
    axl_ssize_t fail_at;   /* return -1 on this call (1-based), 0 to never */
    bool        overclaim; /* report more bytes than were written */
} ChunkSrc;

static axl_ssize_t
chunk_read(void *ctx, void *buf, size_t max)
{
    ChunkSrc *cs = (ChunkSrc *)ctx;
    size_t    n;

    cs->calls++;
    if (cs->fail_at != 0 && (axl_ssize_t)cs->calls == cs->fail_at) {
        return -1;
    }
    n = cs->len - cs->pos;
    if (n > max) {
        n = max;
    }
    if (n > cs->chunk) {
        n = cs->chunk;
    }
    if (n > 0) {
        axl_memcpy(buf, cs->data + cs->pos, n);
    }
    cs->pos += n;
    if (cs->overclaim) {
        /* Widened BEFORE the add, not after: `(axl_ssize_t)(max + 1)`
           computes in size_t and casts the result, which clang-tidy flags as
           either ineffective or lossy -- and on a max of SIZE_MAX it would
           wrap to 0 rather than over-report. */
        return (axl_ssize_t)max + 1;   /* a lie the scanner must refuse */
    }
    return (axl_ssize_t)n;
}

/* Scan @a doc through a pull source delivering @a chunk bytes per read, and
 * render the result exactly as the contiguous path renders it. */
static bool
scan_pull(const char *doc, AxlJsonFlags flags, size_t chunk, char *out,
          size_t size, unsigned *out_calls)
{
    AxlJsonSource  src;
    AxlJsonScanner s;
    ChunkSrc       cs;
    bool           ok;

    axl_memset(&cs, 0, sizeof(cs));
    cs.data  = doc;
    cs.len   = axl_strlen(doc);
    cs.chunk = chunk;

    /* hint deliberately left 0: it is an expected TOTAL, and the window must
       not be sized from it. */
    axl_json_source_init_callback(&src, chunk_read, &cs, 0);
    if (!axl_json_scanner_init(&s, &src, flags)) {
        return false;
    }
    ok = scan_drain(&s, out, size);
    axl_json_scanner_free(&s);
    if (out_calls != NULL) {
        *out_calls = cs.calls;
    }
    return ok;
}

/* A digest of a whole scan: a count per event kind, an FNV-1a over every
 * event's offset, depth and text, and the final error record.
 *
 * The render above is better to read and is what pins small documents. This
 * exists for documents BIGGER than the scanner's window, where a full render
 * would not fit in any reasonable buffer -- and those are the only documents
 * that exercise straddling at all, so it is not an optional extra.
 *
 * Sensitive to one byte moving anywhere, and compared only against the
 * CONTIGUOUS digest of the same document, which the assertions above already
 * pin. */
static bool
scan_digest(AxlJsonScanner *s, char *out, size_t size)
{
    unsigned            counts[10];
    uint32_t            h = 2166136261u;
    AxlJsonEvent        ev;
    const AxlJsonError *e;
    size_t              i;
    size_t              n = 0;

    axl_memset(counts, 0, sizeof(counts));
    while (axl_json_scanner_next(s, &ev)) {
        if ((size_t)ev.kind < sizeof(counts) / sizeof(counts[0])) {
            counts[ev.kind]++;
        }
        h = (h ^ (uint32_t)ev.offset) * 16777619u;
        h = (h ^ ev.depth)            * 16777619u;
        h = (h ^ (uint32_t)ev.len)    * 16777619u;
        for (i = 0; i < ev.len; i++) {
            h = (h ^ (unsigned char)ev.text[i]) * 16777619u;
        }
    }
    for (i = 0; i < sizeof(counts) / sizeof(counts[0]); i++) {
        if (!scan_put(out, size, &n, "%u,", counts[i])) {
            return false;
        }
    }
    e = axl_json_scanner_error(s);
    return scan_put(out, size, &n, "h%08x|e%d@%u %u:%u f%llx", (unsigned)h,
                    (int)e->code, (unsigned)e->offset, e->line, e->column,
                    (unsigned long long)e->missing_flag);
}

/* Compare pull against contiguous for a document too big to render whole.
 *
 * This is the sweep that actually reaches the straddling path. The window is
 * at least a kilobyte and a refill FILLS it, so every fixture shorter than
 * that is delivered entire on the first refill and never straddles anything
 * -- the chunk size only changes how many times the source is called. Proven
 * the hard way: removing the at-end-of-input guard from parse_number left the
 * small-document sweep completely green. */
static void
scan_big_sweep(const char *doc, AxlJsonFlags flags, const char *label)
{
    static const size_t chunks[] = { 1, 7, 64, 997, 4096 };
    AxlJsonSource  src;
    AxlJsonScanner s;
    ChunkSrc       cs;
    char           want[128];
    size_t         i;

    axl_json_source_init_mem(&src, doc, axl_strlen(doc));
    if (!axl_json_scanner_init(&s, &src, flags)
        || !scan_digest(&s, want, sizeof(want))) {
        axl_json_scanner_free(&s);
        test_check(false, label);
        return;
    }
    axl_json_scanner_free(&s);

    for (i = 0; i < sizeof(chunks) / sizeof(chunks[0]); i++) {
        char got[128];
        char msg[192];

        axl_memset(&cs, 0, sizeof(cs));
        cs.data  = doc;
        cs.len   = axl_strlen(doc);
        cs.chunk = chunks[i];
        axl_json_source_init_callback(&src, chunk_read, &cs, 0);
        (void)axl_snprintf(msg, sizeof(msg), "%s [chunk %u]", label,
                           (unsigned)chunks[i]);
        if (!axl_json_scanner_init(&s, &src, flags)) {
            test_check(false, msg);
            continue;
        }
        test_check(scan_digest(&s, got, sizeof(got))
                       && axl_strcmp(got, want) == 0, msg);
        axl_json_scanner_free(&s);
    }
}

/* THE P13 test: the event stream must not depend on the chunking.
 *
 * Swept, not hand-picked. Chunk size 1 is the one that matters -- it puts
 * EVERY byte on a refill boundary, so a leaf that mistakes the end of the
 * window for the end of the input fails immediately and at the first token,
 * rather than at whatever offset a 4096 happens to land on. The larger sizes
 * are there because a bug in the compaction arithmetic hides at chunk 1
 * (nothing ever straddles by more than a byte).
 *
 * The comparison is against the CONTIGUOUS render of the same bytes, so this
 * asserts the property rather than a transcription of it -- and it covers the
 * error offset, line and column too, which is where a botched line/column
 * carry across a refill shows up and nowhere else. */
static void
scan_chunk_sweep(const char *doc, AxlJsonFlags flags, const char *label)
{
    static const size_t chunks[] = { 1, 2, 3, 5, 7, 64, 4096 };
    AxlJsonSource  src;
    AxlJsonScanner s;
    char           want[1024];
    size_t         i;

    axl_json_source_init_mem(&src, doc, axl_strlen(doc));
    if (!axl_json_scanner_init(&s, &src, flags)
        || !scan_drain(&s, want, sizeof(want))) {
        axl_json_scanner_free(&s);
        test_check(false, label);
        return;
    }
    axl_json_scanner_free(&s);

    for (i = 0; i < sizeof(chunks) / sizeof(chunks[0]); i++) {
        char     got[1024];
        char     msg[192];
        unsigned calls = 0;

        (void)axl_snprintf(msg, sizeof(msg), "%s [chunk %u]", label,
                           (unsigned)chunks[i]);
        test_check(scan_pull(doc, flags, chunks[i], got, sizeof(got), &calls)
                       && axl_strcmp(got, want) == 0, msg);
    }
}

static void
test_json_scanner(void)
{
    // --- the shapes, with depth pinned on every event --------------------
    //
    // depth is "containers OUTSIDE this event", so a BEGIN and its matching
    // END report the SAME number. That is the property callers match on, and
    // the opposite convention is equally defensible -- which is exactly why
    // it is pinned here rather than left to whatever the code happened to do.
    scan_check("{\"a\":1}", AXL_JSON_STRICT,
               "{@0/d0 KEY(a)@1/d1 NUM(1)@5/d1 }@6/d0 EOF@7/d0 |e0",
               "scan: object — BEGIN and its END report the same depth");
    scan_check("[1,2]", AXL_JSON_STRICT,
               "[@0/d0 NUM(1)@1/d1 NUM(2)@3/d1 ]@4/d0 EOF@5/d0 |e0",
               "scan: array elements sit one level in");
    scan_check("[]", AXL_JSON_STRICT, "[@0/d0 ]@1/d0 EOF@2/d0 |e0",
               "scan: an EMPTY array closes without a value in between");
    scan_check("{}", AXL_JSON_STRICT, "{@0/d0 }@1/d0 EOF@2/d0 |e0",
               "scan: an EMPTY object closes without a key in between");
    scan_check("42", AXL_JSON_STRICT, "NUM(42)@0/d0 EOF@2/d0 |e0",
               "scan: a bare primitive is a whole document");
    scan_check("{\"a\":{\"b\":[true,null]}}", AXL_JSON_STRICT,
               "{@0/d0 KEY(a)@1/d1 {@5/d1 KEY(b)@6/d2 [@10/d2 "
               "BOOL(true)@11/d3 NUL(null)@16/d3 ]@20/d2 }@21/d1 }@22/d0 "
               "EOF@23/d0 |e0",
               "scan: nesting walks in and back out through the bitmap");

    // --- text spans the INNER content, which is what the decoder takes ----
    scan_check("\"hi\"", AXL_JSON_STRICT, "STR(hi)@0/d0 EOF@4/d0 |e0",
               "scan: a string event's text excludes the quotes, and its "
               "offset points AT the opening quote");

    // --- the dialect reaches the scanner, through the same leaves --------
    scan_check("{a:1,}", AXL_JSON_JSON5,
               "{@0/d0 KEY(a)@1/d1 NUM(1)@3/d1 }@5/d0 EOF@6/d0 |e0",
               "scan: JSON5 unquoted key and trailing comma");
    /* e10 is AXL_JSON_ERR_DIALECT — the numbering is pinned deliberately, so
       inserting an enumerator mid-list has to be looked at rather than
       absorbed. */
    scan_check("{a:1,}", AXL_JSON_STRICT,
               "{@0/d0 |e10@1 1:2 f4",
               "scan: the same document under STRICT stops at the key with a "
               "DIALECT miss");

    // --- a MISSING value is not a trailing comma --------------------------
    //
    // SCAN_ST_VALUE is reached two ways: after `,` in an ARRAY (where a
    // closer really is a trailing comma) and after `:` in an OBJECT (where a
    // value is mandatory). Accepting `}` in that state served only the second
    // case, so `{"a":}` parsed clean under ALLOW_TRAILING_COMMA — and it
    // breaks the event stream's own rule that a KEY is always followed by its
    // value, which the document builder is about to depend on.
    scan_check("{\"a\":}", AXL_JSON_ALLOW_TRAILING_COMMA,
               "{@0/d0 KEY(a)@1/d1 |e3@5 1:6 f0",
               "scan: a missing value is refused even when trailing commas "
               "are allowed");
    scan_check("[1,]", AXL_JSON_ALLOW_TRAILING_COMMA,
               "[@0/d0 NUM(1)@1/d1 ]@3/d0 EOF@4/d0 |e0",
               "scan: an array's trailing comma still closes");
    /* And the object form goes through SCAN_ST_OBJ_KEY, which must ALSO keep
       the parser's recoverable diagnosis when the flag is absent. */
    scan_check("{\"a\":1,}", AXL_JSON_ALLOW_TRAILING_COMMA,
               "{@0/d0 KEY(a)@1/d1 NUM(1)@5/d1 }@7/d0 EOF@8/d0 |e0",
               "scan: an object's trailing comma closes when allowed");
    scan_check("{\"a\":1,}", AXL_JSON_STRICT,
               "{@0/d0 KEY(a)@1/d1 NUM(1)@5/d1 |e10@7 1:8 f2",
               "scan: and without the flag it is a DIALECT miss with a named "
               "remedy, not a bare unexpected byte");

    // --- EOF is a document boundary, not the end of the input ------------
    //
    // The whole reason next() returns true for EV_EOF: an NDJSON caller keeps
    // pulling and gets the second document. Nothing about this needed a flag.
    scan_check("{\"a\":1} {\"b\":2}", AXL_JSON_STRICT,
               "{@0/d0 KEY(a)@1/d1 NUM(1)@5/d1 }@6/d0 EOF@7/d0 "
               "{@8/d0 KEY(b)@9/d1 NUM(2)@13/d1 }@14/d0 EOF@15/d0 |e0",
               "scan: NDJSON — a second document follows the first EOF");

    // --- failures classify through the shared leaves ---------------------
    scan_check("{\"a\":}", AXL_JSON_STRICT, "{@0/d0 KEY(a)@1/d1 |e3@5 1:6 f0",
               "scan: a missing value stops the scan where the byte is");
    scan_check("[1,", AXL_JSON_STRICT, "[@0/d0 NUM(1)@1/d1 |e2@3 1:4 f0",
               "scan: input ending mid-container is INCOMPLETE, not EOF");

    // --- a document deeper than 32 can now be RE-EMITTED ------------------
    //
    // The reader accepted 256 levels; the writer's bitmap was one uint32_t,
    // so anything past 32 parsed fine and then could not be written back.
    // Read-then-re-emit is the writer's main job, so the asymmetry made a
    // whole band of legal documents one-way.
    {
        char           deep[256];
        AxlJsonReader  r;
        AXL_AUTOPTR(AxlString) out = axl_string_new(NULL);
        AxlJsonWriter  w;
        size_t         i;
        const size_t   levels = 40;

        for (i = 0; i < levels; i++) {
            deep[i] = '[';
        }
        deep[levels] = '1';
        for (i = 0; i < levels; i++) {
            deep[levels + 1 + i] = ']';
        }
        deep[levels * 2 + 1] = '\0';

        test_check(axl_json_parse(deep, axl_strlen(deep),
                                        AXL_JSON_DEPTH(64), &r),
                   "writer depth: a 40-level document parses");
        axl_json_writer_init(&w, out, AXL_JSON_STRICT);
        axl_json_write_token(&w, &r, 0);
        axl_json_writer_finish(&w);
        test_check(!axl_json_writer_error(&w),
                   "writer depth: ...and re-emits without hitting the cap");
        test_check(axl_strcmp(axl_string_str(out), deep) == 0,
                   "writer depth: byte-identical round trip at 40 levels");
        axl_json_free(&r);
    }

    // --- running OUT of input is INCOMPLETE, on both faces ---------------
    //
    // `{"a"` ends after the key. The recursive parser tested `pos >= len ||
    // json[pos] != ':'` in one condition and reported UNEXPECTED_BYTE for
    // both, so "there is no byte" was diagnosed as "this byte is wrong". The
    // distinction is load-bearing: INCOMPLETE is the one code more input can
    // clear, which is what P13 resumes from. Found by differential-probing
    // the two faces against each other.
    {
        AxlJsonReader r;

        test_check(!axl_json_parse("{\"a\"", 4, AXL_JSON_STRICT, &r),
                   "parse: a document ending after a key is rejected");
        test_check(axl_json_reader_error(&r)->code == AXL_JSON_ERR_INCOMPLETE,
                   "parse: ...as INCOMPLETE — more bytes could finish it — "
                   "not as an unexpected byte that is not there");
        axl_json_free(&r);
    }
    scan_check("{\"a\"", AXL_JSON_STRICT, "{@0/d0 KEY(a)@1/d1 |e2@4 1:5 f0",
               "scan: and the scanner agrees, which is what makes the two "
               "faces substitutable");

    // --- a scan failure carries a POSITION the formatter can render -------
    //
    // line/column are documented 1-based, and axl_json_error_format() prints
    // them verbatim — leaving them zero rendered "0:0:" with the caret at
    // column 0, from the one face that has no other way to fill them in.
    {
        const char    *doc = "{\n  \"a\": 1,\n  \"b\": @\n}";
        AxlJsonSource  src;
        AxlJsonScanner s;
        AxlJsonEvent   ev;
        char           buf[AXL_JSON_ERROR_BUF_MAX];

        axl_memset(&src, 0, sizeof(src));
        src.data = doc;
        src.len  = axl_strlen(doc);
        (void)axl_json_scanner_init(&s, &src, AXL_JSON_STRICT);
        while (axl_json_scanner_next(&s, &ev)) {
            /* run to the failure */
        }
        test_check(axl_json_scanner_error(&s)->line == 3
                       && axl_json_scanner_error(&s)->column == 8,
                   "scan: a failure records a 1-based line and column, not "
                   "zeros");
        (void)axl_json_error_format(axl_json_scanner_error(&s), doc,
                                    axl_strlen(doc), buf, sizeof(buf));
        test_check(axl_strncmp(buf, "3:8:", 4) == 0,
                   "scan: and the shared formatter renders it, rather than "
                   "printing 0:0:");
        axl_json_scanner_free(&s);
    }

    // --- depth is a policy number now, not a stack budget ----------------
    //
    // 200 levels through a bitmap. Under recursive descent this is ~29 KB of
    // frames; here it is 25 bytes of bitmap, which is the entire argument for
    // the container bitmap.
    {
        char   deep[512];
        size_t i;

        for (i = 0; i < 200; i++) {
            deep[i] = '[';
        }
        deep[200] = '1';
        for (i = 0; i < 200; i++) {
            deep[201 + i] = ']';
        }
        deep[401] = '\0';

        AxlJsonSource  src;
        AxlJsonScanner s;
        AxlJsonEvent   ev;
        uint32_t       deepest = 0;
        int            events  = 0;

        axl_memset(&src, 0, sizeof(src));
        src.data = deep;
        src.len  = axl_strlen(deep);
        test_check(axl_json_scanner_init(&s, &src,
                                         AXL_JSON_DEPTH(256)),
                   "scan: a 256-level bound initializes");
        while (axl_json_scanner_next(&s, &ev)) {
            if (ev.depth > deepest) {
                deepest = ev.depth;
            }
            events++;
        }
        test_check(deepest == 200 && events == 402
                       && axl_json_scanner_error(&s)->code == AXL_JSON_OK,
                   "scan: 200 levels deep with no recursion — the bitmap is "
                   "the whole cost");
        axl_json_scanner_free(&s);
    }

    // --- skip: the reason the streaming face exists ----------------------
    {
        const char    *doc = "{\"skipme\":[1,[2,3],{\"x\":9}],\"want\":7}";
        AxlJsonSource  src;
        AxlJsonScanner s;
        AxlJsonEvent   ev;
        int64_t        got = 0;
        bool           found = false;
        bool           saw_inner = false;

        axl_memset(&src, 0, sizeof(src));
        src.data = doc;
        src.len  = axl_strlen(doc);
        test_check(axl_json_scanner_init(&s, &src, AXL_JSON_STRICT),
                   "scan: skip fixture initializes");
        while (axl_json_scanner_next(&s, &ev)) {
            if (ev.kind != AXL_JSON_EV_KEY) {
                continue;
            }
            if (axl_json_event_equals(&ev, "x")) {
                /* Inside the subtree that was skipped. Seeing it means skip
                   stopped early -- which the "found && got == 7" assertion
                   below CANNOT see, because the outer loop just keeps going
                   and still reaches "want". Sabotaging skip's termination
                   depth proved that gap, so the miss is asserted directly. */
                saw_inner = true;
            }
            if (axl_json_event_equals(&ev, "skipme")) {
                /* Pull the value's BEGIN, then discard the subtree whole. */
                (void)axl_json_scanner_next(&s, &ev);
                test_check(axl_json_scanner_skip(&s),
                           "scan: skip consumes the subtree");
            } else if (axl_json_event_equals(&ev, "want")) {
                char buf[16];

                (void)axl_json_scanner_next(&s, &ev);
                if (axl_json_event_string(&ev, buf, sizeof(buf)) > 0) {
                    got   = (int64_t)(buf[0] - '0');
                    found = true;
                }
            }
        }
        test_check(found && got == 7,
                   "scan: reading one key past a nested subtree still finds "
                   "the key that follows it");
        test_check(!saw_inner,
                   "scan: skip consumed the WHOLE subtree — a key nested "
                   "inside it never surfaced");
        axl_json_scanner_free(&s);
    }

    // --- skip() is a NO-OP on anything but a container --------------------
    //
    // The docstring promises a caller may call it on any event without first
    // asking which kind it was. It did not: `want = depth` means "run to the
    // end of the innermost OPEN container", which is only the subtree when
    // the last event was a BEGIN. After a KEY it swallowed every remaining
    // member — and "pull a key, compare, skip if it is not mine" is the
    // idiom the header sells twice.
    {
        const char    *doc = "{\"a\":1,\"b\":2}";
        AxlJsonSource  src;
        AxlJsonScanner s;
        AxlJsonEvent   ev;
        bool           saw_b = false;

        axl_memset(&src, 0, sizeof(src));
        src.data = doc;
        src.len  = axl_strlen(doc);
        (void)axl_json_scanner_init(&s, &src, AXL_JSON_STRICT);
        (void)axl_json_scanner_next(&s, &ev);            /* { */
        (void)axl_json_scanner_next(&s, &ev);            /* KEY a */
        test_check(axl_json_scanner_skip(&s),
                   "scan: skip after a KEY succeeds");
        while (axl_json_scanner_next(&s, &ev)) {
            if (ev.kind == AXL_JSON_EV_KEY
                && axl_json_event_equals(&ev, "b")) {
                saw_b = true;
            }
        }
        test_check(saw_b,
                   "scan: skip after a KEY consumes only that member — the "
                   "NEXT key is still reachable");
        axl_json_scanner_free(&s);
    }
    {
        /* After a scalar element, skip must not eat the rest of the array. */
        const char    *doc = "[1,2]";
        AxlJsonSource  src;
        AxlJsonScanner s;
        AxlJsonEvent   ev;

        axl_memset(&src, 0, sizeof(src));
        src.data = doc;
        src.len  = axl_strlen(doc);
        (void)axl_json_scanner_init(&s, &src, AXL_JSON_STRICT);
        (void)axl_json_scanner_next(&s, &ev);            /* [ */
        (void)axl_json_scanner_next(&s, &ev);            /* 1 */
        test_check(axl_json_scanner_skip(&s),
                   "scan: skip after a scalar succeeds");
        test_check(axl_json_scanner_next(&s, &ev)
                       && ev.kind == AXL_JSON_EV_NUMBER,
                   "scan: skip after a scalar is a NO-OP — the next element "
                   "is still delivered");
        axl_json_scanner_free(&s);
    }

    // --- event_equals compares DECODED, and cannot truncate --------------
    scan_check("{\"\\u0062\":1}", AXL_JSON_STRICT,
               "{@0/d0 KEY(\\u0062)@1/d1 NUM(1)@10/d1 }@11/d0 EOF@12/d0 |e0",
               "scan: a key's text is its SOURCE spelling, escapes intact");
    {
        AxlJsonSource  src;
        AxlJsonScanner s;
        AxlJsonEvent   ev;
        const char    *doc = "{\"\\u0062\":1}";

        axl_memset(&src, 0, sizeof(src));
        src.data = doc;
        src.len  = axl_strlen(doc);
        (void)axl_json_scanner_init(&s, &src, AXL_JSON_STRICT);
        (void)axl_json_scanner_next(&s, &ev);   /* { */
        (void)axl_json_scanner_next(&s, &ev);   /* the key */
        test_check(axl_json_event_equals(&ev, "b"),
                   "scan: event_equals matches the DECODED name, so an "
                   "escaped key answers to what it names");
        test_check(!axl_json_event_equals(&ev, "\\u0062"),
                   "scan: and NOT to its source spelling");
        test_check(axl_json_event_type(&ev) == AXL_JSON_TYPE_STRING,
                   "scan: a KEY maps into the type vocabulary as a string");
        axl_json_scanner_free(&s);
    }

    // --- a refused init is still SAFE to use and to free -----------------
    {
        AxlJsonScanner s;
        AxlJsonEvent   ev;
        AxlJsonSource  bad;

        axl_memset(&bad, 0, sizeof(bad));   /* neither view nor read fn */
        test_check(!axl_json_scanner_init(&s, &bad, AXL_JSON_STRICT),
                   "scan: a source with no view and no read function is "
                   "refused");
        test_check(axl_json_scanner_error(&s)->code
                       == AXL_JSON_ERR_INVALID_ARGUMENT,
                   "scan: and the refusal is recorded, not just returned");
        test_check(!axl_json_scanner_next(&s, &ev),
                   "scan: next() on a refused scanner reports no event "
                   "rather than reading a garbage window");
        axl_json_scanner_free(&s);   /* must not free a garbage pointer */
        test_survived("scan: freeing a refused scanner is safe");
    }

    // --- P13: a pull source produces the SAME events, at any chunking -----
    //
    // The property, asserted rather than described. Each fixture puts a
    // different leaf's edge case on every possible boundary, because the
    // sweep runs chunk size 1.
    //
    // This is what the at-end-of-INPUT signal exists for. Three leaves treat
    // running out of bytes as a legitimate end of token -- a number, an
    // identifier and a line comment -- so without it `{"n":123}` chunked at 7
    // emits NUM(12) and then meets `3}` as a fresh value, and `true` cut in
    // half is reported as an unexpected byte rather than an incomplete one.
    scan_chunk_sweep("{\"a\":1}", AXL_JSON_STRICT,
                     "pull: the smallest object");
    scan_chunk_sweep("{\"n\":123,\"m\":-4.5e-3}", AXL_JSON_STRICT,
                     "pull: a number is not finished just because the window "
                     "is");
    scan_chunk_sweep("[true,false,null]", AXL_JSON_STRICT,
                     "pull: a keyword split across chunks is INCOMPLETE, not "
                     "an unexpected byte");
    scan_chunk_sweep("{\"s\":\"a\\u00e9b\\tc\"}", AXL_JSON_STRICT,
                     "pull: an escape straddling a boundary survives");
    scan_chunk_sweep("[[1,[2,[3,[4]]]],{\"k\":{\"j\":[]}}]", AXL_JSON_STRICT,
                     "pull: nesting walks in and out identically");
    scan_chunk_sweep("{a:1,b:0x1F,c:NaN,d:Infinity,e:'q',}", AXL_JSON_JSON5,
                     "pull: JSON5 unquoted keys, hex, NaN/Infinity and single "
                     "quotes");
    scan_chunk_sweep("{/*x*/a:1,//y\n b:2}", AXL_JSON_JSON5,
                     "pull: a comment is re-scanned whole rather than resumed "
                     "inside");
    // Non-ASCII on purpose: column counts CHARACTERS, and a multi-byte
    // sequence can straddle a refill. The continuation-byte test is
    // byte-local, so the carried count must come out the same either way --
    // and the error position in the render is the only thing that shows it.
    scan_chunk_sweep("{\"\xC3\xA9\xC3\xA9\":1,\"b\":@}", AXL_JSON_STRICT,
                     "pull: a failure's line and column survive the bytes "
                     "scrolling out of the window");
    scan_chunk_sweep("[1,\n2,\n3,\n@]", AXL_JSON_STRICT,
                     "pull: and the LINE count survives too");
    // Failures must land in the same place, not merely be failures.
    scan_chunk_sweep("{\"a\":1,}", AXL_JSON_STRICT,
                     "pull: a dialect miss keeps its offset and its flag");
    scan_chunk_sweep("{\"a\"", AXL_JSON_STRICT,
                     "pull: a truncated document is INCOMPLETE at the end of "
                     "the input");
    scan_chunk_sweep("   ", AXL_JSON_STRICT,
                     "pull: a whitespace-only input exhausts cleanly");

    // --- P13: the source is asked more than once --------------------------
    //
    // Without this the sweep proves nothing: a chunk larger than the document
    // makes the pull path deliver everything in one read, which is the
    // contiguous path wearing a callback.
    {
        char     out[1024];
        unsigned calls = 0;

        test_check(scan_pull("{\"a\":1,\"b\":2}", AXL_JSON_STRICT, 1, out,
                             sizeof(out), &calls)
                       && calls > 1,
                   "pull: a one-byte-at-a-time source is genuinely read many "
                   "times");
    }

    // --- P13: the input channel's own failures ----------------------------
    {
        AxlJsonSource  src;
        AxlJsonScanner s;
        ChunkSrc       cs;
        char           out[256];

        // A read error is AXL_JSON_ERR_IO (e11), never INCOMPLETE: "the
        // socket died" and "the document was truncated" are opposite
        // instructions, which is why AxlJsonReadFn returns a SIGNED count.
        axl_memset(&cs, 0, sizeof(cs));
        cs.data = "{\"a\":1}";
        cs.len  = 7;
        cs.chunk = 2;
        cs.fail_at = 1;
        axl_json_source_init_callback(&src, chunk_read, &cs, 0);
        test_check(axl_json_scanner_init(&s, &src, AXL_JSON_STRICT),
                   "pull: init does not read, so a doomed source still "
                   "initializes");
        test_check(scan_drain(&s, out, sizeof(out))
                       && axl_strcmp(out, "|e11@0 1:1 f0") == 0,
                   "pull: a read error is IO, not INCOMPLETE");
        axl_json_scanner_free(&s);

        // A source claiming more bytes than it was offered would have written
        // past the window. Refused, not believed.
        axl_memset(&cs, 0, sizeof(cs));
        cs.data = "{\"a\":1}";
        cs.len  = 7;
        cs.chunk = 2;
        cs.overclaim = true;
        axl_json_source_init_callback(&src, chunk_read, &cs, 0);
        (void)axl_json_scanner_init(&s, &src, AXL_JSON_STRICT);
        test_check(scan_drain(&s, out, sizeof(out))
                       && axl_strcmp(out, "|e11@0 1:1 f0") == 0,
                   "pull: a read reporting more than it was offered is "
                   "refused rather than trusted");
        axl_json_scanner_free(&s);
    }

    // --- P13: documents BIGGER than the window, which is where tokens
    //          actually straddle a refill ------------------------------------
    //
    // Everything above is smaller than the window, so the first refill
    // delivers it whole and nothing straddles. These are the assertions that
    // reach the re-scan path, and the ones that fail when a leaf mistakes the
    // end of the window for the end of the input.
    //
    // Built here rather than written out: the point is the LENGTH, and the
    // lengths are chosen to walk a token across the window boundary rather
    // than to land on it once.
    {
        static char big[6144];
        size_t      pad;

        // 1. A single token far longer than the window, so the window has to
        //    GROW rather than merely compact. This is the one case where a
        //    compaction that freed nothing must double the buffer.
        {
            size_t n = 0;
            size_t i;

            n += (size_t)axl_snprintf(big + n, sizeof(big) - n, "{\"k\":\"");
            for (i = 0; i < 3000; i++) {
                big[n++] = (char)('a' + (i % 26));
            }
            n += (size_t)axl_snprintf(big + n, sizeof(big) - n, "\"}");
            big[n] = '\0';
            scan_big_sweep(big, AXL_JSON_STRICT,
                           "pull: a 3000-byte string forces the window to "
                           "grow, and reads back identically");
        }

        // 2. Many SMALL tokens spread past several window boundaries, so
        //    numbers, keys and punctuation each land across a refill at some
        //    point. The odd element width is deliberate -- a round one would
        //    align every boundary to the same place in the pattern.
        {
            size_t n = 0;
            int    i;

            big[n++] = '[';
            for (i = 0; i < 400; i++) {
                n += (size_t)axl_snprintf(big + n, sizeof(big) - n,
                                          "%s{\"k%d\":%d.%de%d}",
                                          i ? "," : "", i, i, i % 97, i % 7);
            }
            big[n++] = ']';
            big[n]   = '\0';
            scan_big_sweep(big, AXL_JSON_STRICT,
                           "pull: hundreds of small tokens across many "
                           "refills, every one re-scanned correctly");
        }

        // 3. Keywords and JSON5 shapes pushed across a boundary by padding
        //    that grows one byte at a time, so `true`, `NaN`, `Infinity` and
        //    an unquoted key each straddle the window at SOME padding length.
        //    A single fixture would only ever cut one of them.
        //
        //    The RANGE is the whole test. It was 1016..1032, which sounds
        //    like "around the 1024 boundary" and is not: at those paddings
        //    the first boundary lands inside the leading STRING or inside
        //    `true`, and after the first refill the base jumps to the
        //    straddling token so the remaining ~50 bytes all fit. Every other
        //    keyword was never cut at any padding in that range. `NaN` needs
        //    pad ~1002 and `-Infinity` pad 983..990 -- and widening to cover
        //    them turned this RED on 8 paddings, which is how two shipped
        //    bugs were found. Do not narrow it back.
        for (pad = 960; pad <= 1040; pad++) {
            char   msg[128];
            size_t n = 0;
            size_t i;

            big[n++] = '[';
            big[n++] = '"';
            for (i = 0; i < pad; i++) {
                big[n++] = 'p';
            }
            big[n++] = '"';
            n += (size_t)axl_snprintf(big + n, sizeof(big) - n,
                                      ",true,false,null,NaN,Infinity,"
                                      "-Infinity,0x1F,{u:'v'},.5]");
            big[n] = '\0';
            (void)axl_snprintf(msg, sizeof(msg),
                               "pull: keywords straddle the window at pad %u",
                               (unsigned)pad);
            {
                AxlJsonSource  src;
                AxlJsonScanner s;
                ChunkSrc       cs;
                char           want[128];
                char           got[128];

                axl_json_source_init_mem(&src, big, n);
                (void)axl_json_scanner_init(&s, &src, AXL_JSON_JSON5);
                if (!scan_digest(&s, want, sizeof(want))) {
                    axl_json_scanner_free(&s);
                    test_check(false, msg);
                    continue;
                }
                axl_json_scanner_free(&s);

                axl_memset(&cs, 0, sizeof(cs));
                cs.data  = big;
                cs.len   = n;
                cs.chunk = 4096;
                axl_json_source_init_callback(&src, chunk_read, &cs, 0);
                (void)axl_json_scanner_init(&s, &src, AXL_JSON_JSON5);
                test_check(scan_digest(&s, got, sizeof(got))
                               && axl_strcmp(got, want) == 0, msg);
                axl_json_scanner_free(&s);
            }
        }

        // 4. A comment longer than the window. It is skipped, not emitted, so
        //    it is the one "token" whose size the bound is stated for.
        {
            size_t n = 0;
            size_t i;

            n += (size_t)axl_snprintf(big + n, sizeof(big) - n, "[1,/*");
            for (i = 0; i < 2500; i++) {
                big[n++] = 'c';
            }
            n += (size_t)axl_snprintf(big + n, sizeof(big) - n, "*/2]");
            big[n] = '\0';
            scan_big_sweep(big, AXL_JSON_JSON5,
                           "pull: a comment longer than the window is skipped "
                           "whole, never resumed inside");
        }
    }

    // --- P13: a source carrying BOTH a view and a read function -----------
    //
    // Not a third mode: the header says `data` selects the view. Left
    // ambiguous it was a NULL dereference -- the window path saw a read
    // function and refilled against a buffer the contiguous path never
    // allocated -- and where it survived it scanned the document TWICE, once
    // from the view and again from the callback as a second NDJSON document.
    // The pre-P13 code could not reach this because it refused every pull
    // source outright.
    {
        AxlJsonSource  src;
        AxlJsonScanner s;
        ChunkSrc       cs;
        char           out[256];
        char           want[256];

        axl_memset(&cs, 0, sizeof(cs));
        cs.data  = "[true";      /* cut mid-keyword, to force a refill */
        cs.len   = 5;
        cs.chunk = 2;

        axl_json_source_init_mem(&src, "[true", 5);
        (void)axl_json_scanner_init(&s, &src, AXL_JSON_STRICT);
        (void)scan_drain(&s, want, sizeof(want));
        axl_json_scanner_free(&s);

        src.read = chunk_read;   /* both fields set, on purpose */
        src.ctx  = &cs;
        (void)axl_json_scanner_init(&s, &src, AXL_JSON_STRICT);
        test_check(scan_drain(&s, out, sizeof(out))
                       && axl_strcmp(out, want) == 0,
                   "pull: a source with BOTH a view and a read function is "
                   "the view, not a crash and not two documents");
        axl_json_scanner_free(&s);
    }

    // --- P13: an I/O failure AFTER the window has moved -------------------
    //
    // The earlier IO row fails on the FIRST read, where base is still 0 and
    // the offset arithmetic cannot be wrong. This one fails on the SECOND,
    // after compaction has advanced base -- the only way to catch a position
    // rebased against the wrong origin.
    //
    // Laid out so the answer is computable rather than observed: `[`, then
    // 1000 spaces (settled by skip_ws, so compaction drops them and base
    // becomes exactly 1001), then a number long enough to run off the 1024
    // window. The refill that number provokes is where read #2 fails, and
    // s->pos is 0 by then -- so a correct report is 1001 and a report of 0
    // is base being lost.
    {
        AxlJsonSource  src;
        AxlJsonScanner s;
        ChunkSrc       cs;
        static char    doc[1200];
        char           out[256];
        size_t         n = 0;
        size_t         i;

        doc[n++] = '[';
        for (i = 0; i < 1000; i++) {
            doc[n++] = ' ';
        }
        for (i = 0; i < 100; i++) {
            doc[n++] = (char)('0' + (i % 10));
        }
        doc[n++] = ']';
        doc[n]   = '\0';

        axl_memset(&cs, 0, sizeof(cs));
        cs.data    = doc;
        cs.len     = n;
        cs.chunk   = 4096;   /* one read fills the window; the SECOND fails */
        cs.fail_at = 2;
        axl_json_source_init_callback(&src, chunk_read, &cs, 0);
        (void)axl_json_scanner_init(&s, &src, AXL_JSON_STRICT);
        test_check(scan_drain(&s, out, sizeof(out))
                       && axl_strcmp(out, "[@0/d0 |e11@1001 1:1002 f0") == 0,
                   "pull: an I/O failure after compaction reports the input "
                   "offset, not one rebased against a moved window");
        axl_json_scanner_free(&s);
    }

    // --- P13: the BOUND, measured rather than asserted in prose -----------
    //
    // The header promises O(largest single token), not O(document). Peak
    // allocation is the only way to check that, and whitespace is the case
    // that used to break it: skip_ws anchored a refill at the START of the
    // insignificant run, so a stream of spaces doubled the window forever.
    // 200 KB of spaces reached a 256 KB window.
    {
        AxlJsonSource  src;
        AxlJsonScanner s;
        ChunkSrc       cs;
        static char    spaces[200000];
        AxlJsonEvent   ev;
        AxlMemStats    st;
        size_t         before;
        size_t         peak;

        axl_memset(spaces, ' ', sizeof(spaces) - 3);
        spaces[sizeof(spaces) - 3] = '4';
        spaces[sizeof(spaces) - 2] = '2';
        spaces[sizeof(spaces) - 1] = '\0';

        axl_memset(&cs, 0, sizeof(cs));
        cs.data  = spaces;
        cs.len   = axl_strlen(spaces);
        cs.chunk = 4096;
        axl_json_source_init_callback(&src, chunk_read, &cs, 0);
        (void)axl_json_scanner_init(&s, &src, AXL_JSON_STRICT);

        axl_mem_get_stats(&st);
        before = st.bytes;
        peak   = before;
        while (axl_json_scanner_next(&s, &ev)) {
            axl_mem_get_stats(&st);
            if (st.bytes > peak) {
                peak = st.bytes;
            }
        }
        test_check(axl_json_scanner_error(&s)->code == AXL_JSON_OK
                       && peak - before < 65536,
                   "pull: 200 KB of leading whitespace does not grow the "
                   "window -- settled whitespace is DROPPED, not re-scanned");
        axl_json_scanner_free(&s);
    }

    // --- P13: consumed() over a pull source -------------------------------
    //
    // It had no test at all, in either mode, despite a header contract making
    // specific claims. It counts what the GRAMMAR consumed, which is less
    // than what was pulled.
    {
        AxlJsonSource  src;
        AxlJsonScanner s;
        ChunkSrc       cs;
        AxlJsonEvent   ev;

        axl_memset(&cs, 0, sizeof(cs));
        cs.data  = "{\"a\":1}   ";
        cs.len   = 10;
        cs.chunk = 3;
        axl_json_source_init_callback(&src, chunk_read, &cs, 0);
        (void)axl_json_scanner_init(&s, &src, AXL_JSON_STRICT);
        while (axl_json_scanner_next(&s, &ev)
               && ev.kind != AXL_JSON_EV_EOF) {
            /* run to the document boundary */
        }
        test_check(axl_json_scanner_consumed(&s) == 7,
                   "pull: consumed() stops at the root value, excluding the "
                   "trailing whitespace the scanner had already pulled");
        axl_json_scanner_free(&s);
    }

    // --- P13: an EMPTY pull source matches an empty contiguous one --------
    //
    // The scanner's own contract says a false return with AXL_JSON_OK means
    // "genuinely exhausted", and that must not depend on which mode delivered
    // the nothing. It briefly did: the read loop stops at a FULL window
    // without asking again, so an input ending exactly on a boundary had not
    // latched end-of-input, and the next refill reported "no progress" while
    // holding an INCOMPLETE one re-run away from being OK.
    //
    // (The WHOLE-DOCUMENT face still answers INCOMPLETE for an empty stream.
    // That is axl_json_parse_source's rule -- a document with no value in it
    // -- not the scanner's, and the two are documented apart.)
    {
        AxlJsonSource  src;
        AxlJsonScanner s;
        ChunkSrc       cs;
        char           out[128];

        axl_memset(&cs, 0, sizeof(cs));
        cs.data  = "";
        cs.len   = 0;
        cs.chunk = 8;
        axl_json_source_init_callback(&src, chunk_read, &cs, 0);
        (void)axl_json_scanner_init(&s, &src, AXL_JSON_STRICT);
        test_check(scan_drain(&s, out, sizeof(out))
                       && axl_strcmp(out, "|e0") == 0,
                   "pull: an empty source exhausts cleanly, exactly as an "
                   "empty contiguous one does");
        axl_json_scanner_free(&s);
    }

    // Input ending EXACTLY on a window boundary. The shape that lost whole
    // documents: a root-level number of exactly 1024 digits produced no
    // events at all, because end-of-input had not been latched when the
    // window filled and the next refill called that "no progress".
    {
        static char digits[1200];
        size_t      i;

        for (i = 0; i < 1024; i++) {
            digits[i] = (char)('1' + (i % 9));
        }
        digits[1024] = '\0';
        scan_big_sweep(digits, AXL_JSON_STRICT,
                       "pull: an input ending exactly on a window boundary "
                       "still produces its events");
    }

    // --- P13: NDJSON over a pull source -----------------------------------
    //
    // EV_EOF is a document boundary, so the second document simply follows
    // the first -- no flag, and now no contiguous buffer either.
    scan_chunk_sweep("{\"a\":1} {\"b\":2} 3", AXL_JSON_STRICT,
                     "pull: concatenated documents keep coming, with an EOF "
                     "between each");
}
// ---------------------------------------------------------------------------
// P12e — the whole-document face over the scanner
//
// axl_json_parse is to become a scan loop plus a builder stack. Three
// things that makes newly fragile, none of which the assertions already here
// were written to catch:
//
//  1. The TRAILING REGION is policy the scanner deliberately does not have.
//     After the root value the scanner reports a document BOUNDARY and stops
//     judging; "only insignificant text remains" has to be asked separately,
//     and the two ways of failing that question are different answers a
//     caller acts on differently -- a skip_ws failure names a DIALECT flag to
//     pass, a second document is TRAILING and resumable. From outside the
//     event stream they are indistinguishable, which is exactly why they are
//     pinned here rather than left to the implementation.
//
//  2. A document with NO root value at all. The scanner calls that clean
//     exhaustion (it is what terminates an NDJSON loop) and returns false
//     with AXL_JSON_OK; the whole-document face must call it INCOMPLETE. A
//     face that simply propagated the scanner's verdict would ACCEPT a
//     whitespace-only document -- silently, with no tokens. The face must
//     also still CONSULT that verdict, or it converts every failure BEFORE
//     the root value into a bogus INCOMPLETE.
//
//  3. `start`, `end` and `size`, which the event stream states nowhere. Leaf
//     bounds come from ev.text/ev.len, a container's from its BEGIN and END
//     offsets, and `size` from counting children.
//
// Errors are pinned as an exact whole-record render rather than by code
// alone, because the offset is what a caller resumes or points a caret from
// and a code-only assertion lets it drift.
// ---------------------------------------------------------------------------

/* Render a REJECTED parse as `@<offset> <terse diagnostic>`.
 *
 * The diagnostic comes from axl_json_error_format() with no document, which
 * renders line, column, the code's own words and -- for a dialect miss -- the
 * name of the flag that would have accepted the input. Offset is prepended
 * because that is the one field the formatter does not show and the one
 * axl_json_reader_consumed() hands to the next parse.
 *
 * Offset and column are NOT independent on a single-line ASCII document:
 * both faces DERIVE line and column from the offset, so there `column ==
 * offset + 1` and the `@` prefix discriminates nothing. It earns its place on
 * the multi-line row below, and it becomes load-bearing in P13, where offset
 * carries the window base while line and column are counted window-relative.
 *
 * The reader is deliberately NOT freed on this path. A rejected parse must
 * own nothing, so there is nothing to release -- and freeing anyway would let
 * a future builder that leaked its token array into a failed reader pass the
 * teardown leak gate. */
static bool
err_render(const AxlJsonReader *r, char *out, size_t size)
{
    char terse[AXL_JSON_ERROR_BUF_MAX];
    int  n;

    if (axl_json_error_format(axl_json_reader_error(r), NULL, 0, terse,
                              sizeof(terse)) < 0) {
        return false;
    }
    /* Not `n += axl_snprintf(...)`: it returns the length it WOULD have
       written, so accumulating walks past the buffer. Formatted in one call
       instead, and the width checked afterwards. */
    n = axl_snprintf(out, size, "@%u %s",
                     (unsigned)axl_json_reader_consumed(r), terse);
    return n > 0 && (size_t)n < size;
}

static void
parse_err_check(const char *doc, AxlJsonFlags flags, const char *want,
                const char *msg)
{
    AxlJsonReader r;
    char          out[AXL_JSON_ERROR_BUF_MAX + 32];

    if (axl_json_parse(doc, axl_strlen(doc), flags, &r)) {
        axl_json_free(&r);
        test_check(false, msg);   /* it was supposed to be REJECTED */
        return;
    }
    test_check(err_render(&r, out, sizeof(out))
                   && axl_strcmp(out, want) == 0, msg);
}

/* Render a parsed document's STRUCTURE by walking it with the public
 * iterators, then compare the whole line exactly.
 *
 * Not axl_json_write_token(): that walks by token type and size, so it cannot
 * see a container's `end` at all. The iterators CAN -- axl_json_object_next()
 * and axl_json_array_next() skip a member's nested children by comparing each
 * following token's start against the value's `end`.
 *
 * What that catches is asymmetric, and worth stating so the rows below do not
 * claim more than they prove. An `end` one byte too LONG swallows the next
 * sibling. An `end` one byte too SHORT is invisible for a non-root container:
 * the skip loop breaks on `start >= end`, and the byte at `end - 1` is the
 * closing delimiter, where no token can begin. The ROOT container's `end` is
 * pinned directly, through axl_json_reader_consumed() in the `|c` suffix, so
 * a UNIFORM off-by-one is caught there.
 *
 * `size` shows up as the member count, and only when the container is
 * FOLLOWED by something: at the end of a token array both iterators run out
 * and stop in the right place whatever the count says.
 */
static bool
shape_scalar(const AxlJsonReader *r, char *out, size_t size, size_t *n)
{
    char buf[64];

    switch (axl_json_value_type(r)) {
    case AXL_JSON_TYPE_STRING:
        /* axl_json_value_string() TRUNCATES and still returns true, so a
           value that did not fit would render short and then compare equal to
           a `want` pasted from that same short output -- a fixture pinning
           its own bug. Refused instead. */
        if (!axl_json_value_string(r, buf, sizeof(buf))
            || axl_strlen(buf) >= sizeof(buf) - 1) {
            return false;
        }
        return scan_put(out, size, n, "\"%s\"", buf);
    case AXL_JSON_TYPE_NUMBER:
        /* The literal, VERBATIM. Reading it as a double would hide a token
           whose [start,end) drifted onto a neighbouring byte. This accessor
           REFUSES rather than truncating, so it needs no length guard. */
        if (!axl_json_value_number_str(r, buf, sizeof(buf))) {
            return false;
        }
        return scan_put(out, size, n, "%s", buf);
    case AXL_JSON_TYPE_BOOL: {
        bool v = false;

        if (!axl_json_value_bool(r, &v)) {
            return false;
        }
        return scan_put(out, size, n, "%s", v ? "true" : "false");
    }
    case AXL_JSON_TYPE_NULL:
        return scan_put(out, size, n, "null");
    default:
        return false;
    }
}

static bool
shape_render(const AxlJsonReader *r, char *out, size_t size, size_t *n)
{
    AxlJsonType t = axl_json_value_type(r);

    if (t == AXL_JSON_TYPE_OBJECT) {
        AxlJsonObjectIter it;
        AxlJsonReader     v;
        char              key[64];
        bool              first = true;

        if (!scan_put(out, size, n, "{")
            || !axl_json_value_object_begin(r, &it)) {
            return false;
        }
        while (axl_json_object_next(&it, key, sizeof(key), &v)) {
            /* A key too long is truncated and the pair still yielded, with
               the truncation reported ONLY here -- see the accessor's own
               warning that a prefix cannot be recognised as short from its
               own contents. Unchecked, an oversized key renders as a shorter
               one and the row silently pins the wrong name.

               Keys are written unquoted, so this render is ambiguous for a
               key containing `,`, `:` or `}`. No fixture below uses one; a
               row that needs one should quote them here first. */
            if (axl_json_object_iter_error(&it)->code != AXL_JSON_OK
                || !scan_put(out, size, n, "%s%s:", first ? "" : ",", key)
                || !shape_render(&v, out, size, n)) {
                return false;
            }
            first = false;
        }
        return scan_put(out, size, n, "}");
    }
    if (t == AXL_JSON_TYPE_ARRAY) {
        AxlJsonArrayIter it;
        AxlJsonReader    e;
        bool             first = true;

        if (!scan_put(out, size, n, "[")
            || !axl_json_value_array_begin(r, &it)) {
            return false;
        }
        while (axl_json_array_next(&it, &e)) {
            if (!scan_put(out, size, n, "%s", first ? "" : ",")
                || !shape_render(&e, out, size, n)) {
                return false;
            }
            first = false;
        }
        return scan_put(out, size, n, "]");
    }
    return shape_scalar(r, out, size, n);
}

static void
shape_check(const char *doc, AxlJsonFlags flags, const char *want,
            const char *msg)
{
    AxlJsonReader r;
    char          out[512];
    size_t        n = 0;

    out[0] = '\0';
    if (!axl_json_parse(doc, axl_strlen(doc), flags, &r)) {
        test_check(false, msg);   /* it was supposed to PARSE */
        return;
    }
    if (!shape_render(&r, out, sizeof(out), &n)
        || !scan_put(out, sizeof(out), &n, "|c%u",
                     (unsigned)axl_json_reader_consumed(&r))) {
        axl_json_free(&r);
        test_check(false, msg);
        return;
    }
    test_check(axl_strcmp(out, want) == 0, msg);
    axl_json_free(&r);
}

static void
test_json_document_face(void)
{
    // --- the trailing region: two failures the event stream cannot tell apart
    //
    // Both stop with the root value already complete. What separates them is
    // whether skip_ws itself refused (a DIALECT miss, which names a flag and
    // is recoverable) or whether insignificant text simply ran out and
    // something else was there (TRAILING, resumable via
    // axl_json_reader_consumed). Merging them would hand a caller the half it
    // cannot act on.
    parse_err_check("1 //c", AXL_JSON_STRICT,
                    "@2 1:3: feature needs a dialect flag "
                    "(pass AXL_JSON_ALLOW_COMMENTS)",
                    "docface: a comment after the root names ALLOW_COMMENTS, "
                    "not 'trailing content'");
    parse_err_check("1 ,", AXL_JSON_STRICT, "@2 1:3: trailing content",
                    "docface: a byte no document can start with is TRAILING");
    parse_err_check("1 2", AXL_JSON_STRICT, "@2 1:3: trailing content",
                    "docface: a well-formed SECOND document is TRAILING too "
                    "-- the scanner would happily scan it");
    // The row the `@offset` prefix exists for: three lines, so the offset and
    // the column are no longer the same number off by one. Every other row
    // here is single-line ASCII, where they move in lockstep and the prefix
    // adds nothing.
    parse_err_check("{\n  \"a\": 1\n} x", AXL_JSON_STRICT,
                    "@13 3:3: trailing content",
                    "docface: offset counts BYTES from the start of the "
                    "input while column restarts each line");

    // The same trailing region, this time LEGAL. A comment there is not junk
    // when the dialect allows it, and consumed() still reports where the
    // VALUE ended rather than where the comment did.
    shape_check("1 //c", AXL_JSON_JSON5, "1|c1",
                "docface: a permitted trailing comment is skipped, and "
                "consumed() still stops at the value");
    shape_check("1 /*c*/ ", AXL_JSON_JSON5, "1|c1",
                "docface: a permitted trailing BLOCK comment likewise");
    // The one that a `pos != len` check alone gets wrong. Spelled `1 /*`
    // rather than `1 /*c` deliberately: a comment-open at the very END
    // advances pos to exactly len, so "is there anything left?" answers NO
    // and the truncated document is ACCEPTED unless skip_ws's own refusal is
    // checked. With a byte after the `/*` the position check catches it
    // anyway, which makes that spelling a passenger -- verified by sabotage,
    // where only this one notices.
    parse_err_check("1 /*", AXL_JSON_JSON5, "@4 1:5: input ended early",
                    "docface: a trailing comment-open at end of input is "
                    "INCOMPLETE, not a silently accepted document");

    // --- no root value at all --------------------------------------------
    //
    // The scanner calls this clean exhaustion and returns false with OK,
    // because that is what ends an NDJSON loop. The whole-document face must
    // NOT propagate that verdict: there is no value, so there is no document.
    parse_err_check("   ", AXL_JSON_STRICT, "@3 1:4: input ended early",
                    "docface: a whitespace-only document is INCOMPLETE, not "
                    "an empty success");
    parse_err_check("/*c*/", AXL_JSON_JSON5, "@5 1:6: input ended early",
                    "docface: a comment-only document is INCOMPLETE");
    // ...and the mirror image, which is the half a face that synthesises
    // INCOMPLETE from "no root token" would destroy. Here the scan fails
    // BEFORE any value, so the scanner's own classification is the good one
    // and must be consulted rather than overwritten. Both rows above would
    // still pass if it were not, because for them INCOMPLETE is the right
    // answer by luck.
    parse_err_check("//c", AXL_JSON_STRICT,
                    "@0 1:1: feature needs a dialect flag "
                    "(pass AXL_JSON_ALLOW_COMMENTS)",
                    "docface: a comment BEFORE the root still names its "
                    "flag, rather than becoming a bogus INCOMPLETE");

    // --- interior failures keep their position ---------------------------
    //
    // The two faces reach these through structurally different code -- the C
    // stack unwinding through lex_fail, versus scan_fail over an explicit
    // state machine -- and nothing else in the suite pins where they land.
    parse_err_check("{\"a\":1", AXL_JSON_STRICT, "@6 1:7: input ended early",
                    "docface: a truncated object reports the end of input");
    parse_err_check("{\"a\":}", AXL_JSON_STRICT, "@5 1:6: unexpected byte",
                    "docface: a missing value points AT the brace that "
                    "arrived instead of it");
    parse_err_check("[1,", AXL_JSON_STRICT, "@3 1:4: input ended early",
                    "docface: a truncated array likewise");

    // --- the builder reconstructs `start`, `end` and `size` ---------------
    shape_check("{\"a\":1}", AXL_JSON_STRICT, "{a:1}|c7",
                "docface: an object round-trips through the builder");
    shape_check("[1,2,3]", AXL_JSON_STRICT, "[1,2,3]|c7",
                "docface: an array's elements survive in order -- an "
                "under-counted size drops the tail");
    shape_check("{}", AXL_JSON_STRICT, "{}|c2",
                "docface: an empty object is one token, and its end is past "
                "the brace");
    shape_check("[]", AXL_JSON_STRICT, "[]|c2",
                "docface: an empty array likewise");
    // An EMPTY container's size is only observable once something follows it:
    // alone at the end of a token array, the iterators run out and stop in
    // the right place whatever the count says.
    shape_check("{\"a\":{},\"b\":1}", AXL_JSON_STRICT, "{a:{},b:1}|c14",
                "docface: an over-counted EMPTY object would swallow the "
                "pair after it");
    // The shape that catches a container `end` one byte too long: the
    // iterator skips a member's nested children by comparing their start
    // against the VALUE's end, so the pair AFTER a nested container is the
    // first casualty.
    shape_check("{\"a\":{\"b\":[1,{\"c\":2}]},\"d\":3}", AXL_JSON_STRICT,
                "{a:{b:[1,{c:2}]},d:3}|c29",
                "docface: a member FOLLOWING a nested container is still "
                "found, which is what a container's `end` is for");
    shape_check("[[1,2],[3],[]]", AXL_JSON_STRICT, "[[1,2],[3],[]]|c14",
                "docface: sibling arrays do not bleed into one another");
    // Object and array counting are different rules on the same field: an
    // object counts KEYS, an array counts ELEMENTS. A builder that increments
    // on every child gives the inner object a size of 4.
    //
    // The inner object must be FOLLOWED by a sibling for that to be visible.
    // On `{"a":1,"b":2}` alone an over-count is invisible -- the iterator
    // runs out of tokens and stops at the right place anyway -- so that
    // spelling catches only an under-count while claiming to catch both.
    // Verified by sabotage: `size = pair_count * 2` leaves the flat document
    // passing and makes this one yield `z` as a third member of the inner
    // object.
    shape_check("{\"o\":{\"a\":1,\"b\":2},\"z\":9}", AXL_JSON_STRICT,
                "{o:{a:1,b:2},z:9}|c25",
                "docface: an object's size counts members, not tokens -- an "
                "over-count walks into the following sibling");

    // --- the token array GROWS mid-document, with a container still open --
    //
    // The array starts at 16 entries and doubles by allocating a new block
    // and freeing the old one. This document reaches 20 tokens, and the
    // reallocation lands while the inner object is OPEN -- so a builder that
    // remembered its containers as AxlJsonTok POINTERS rather than indices
    // patches freed memory here, and nowhere else in this file: every other
    // fixture stays under 16 tokens.
    shape_check("[0,1,2,3,4,5,6,7,8,9,10,11,{\"k\":1,\"m\":2},13,14]",
                AXL_JSON_STRICT,
                "[0,1,2,3,4,5,6,7,8,9,10,11,{k:1,m:2},13,14]|c47",
                "docface: a container held open across the token array's "
                "growth is still patched correctly");

    // --- dialect features reach the builder through the same events -------
    //
    // The trailing comma must not be counted as an element, and the array it
    // closes is FOLLOWED by a sibling so an over-count is visible: at the end
    // of the token array it would not be.
    shape_check("{a:[1,],b:2}", AXL_JSON_JSON5, "{a:[1],b:2}|c12",
                "docface: a JSON5 trailing comma adds no element, and an "
                "over-count would yield the next key as one");
    shape_check("{a:1,b:'x',c:0xFF,d:NaN,}", AXL_JSON_JSON5,
                "{a:1,b:\"x\",c:0xFF,d:NaN}|c25",
                "docface: unquoted keys, single quotes, hex and NaN all "
                "build the same tree a quoted document would");
    // A string token's [start,end) brackets the INNER content, so its `end`
    // is the index OF the closing quote while a container's is one PAST the
    // brace. The builder reconstructs both from ev.text, which is the only
    // field carrying the distinction -- ev.offset points at the quote.
    shape_check("\"hi\"", AXL_JSON_STRICT, "\"hi\"|c4",
                "docface: a STRING root counts its closing quote");
    shape_check("'hi'", AXL_JSON_JSON5, "\"hi\"|c4",
                "docface: and a single-quoted root, whose closing quote is a "
                "different byte");
    shape_check("{\"a\":\"\",\"b\":\"x\"}", AXL_JSON_STRICT,
                "{a:\"\",b:\"x\"}|c16",
                "docface: an EMPTY string is a zero-length span, not a "
                "missing one");
    // Source length and decoded length differ here, in both directions: the
    // key is 6 source bytes decoding to 1 character, the value 4 decoding to
    // 3. A builder sizing a span from the DECODED length lands mid-document.
    shape_check("{\"\\u0041\":\"a\\tb\"}", AXL_JSON_STRICT,
                "{A:\"a\tb\"}|c17",
                "docface: a span is measured in SOURCE bytes, not decoded "
                "ones");

    // --- the post-passes still see a correctly built array ----------------
    //
    // AXL_JSON_REJECT_DUPLICATES walks the finished tokens through
    // axl_json_tok_subtree_end, which consumes `size` RECURSIVELY -- a
    // stricter oracle than the iterators, and one that mis-pairs keys in
    // exactly the last-child positions where they are blind.
    parse_err_check("{\"o\":{\"a\":1,\"a\":2},\"z\":3}",
                    AXL_JSON_STRICT | AXL_JSON_REJECT_DUPLICATES,
                    "@13 1:14: duplicate key",
                    "docface: the duplicate-key pass walks the built array by "
                    "size and finds the SECOND of the offending pair");
    shape_check("{\"o\":{\"a\":1,\"b\":2},\"z\":3}",
                AXL_JSON_STRICT | AXL_JSON_REJECT_DUPLICATES,
                "{o:{a:1,b:2},z:3}|c25",
                "docface: and accepts the same shape with distinct keys, so "
                "the row above is not passing on a mis-walk");

    // --- the depth bound is reported where the container OPENS ------------
    parse_err_check("[[[[[1]]]]]", AXL_JSON_STRICT | AXL_JSON_DEPTH(2),
                    "@2 1:3: nesting too deep",
                    "docface: DEPTH points at the bracket that exceeded the "
                    "bound, not at the value inside it");

    // --- running out of memory still reports a POSITION -------------------
    //
    // The face makes TWO allocations, where the recursive parser made one:
    // the builder stack up front, then the token array as it doubles. Both
    // are covered here, because the second one reported line 0 column 0 --
    // AxlJsonError documents both as 1-based and axl_json_error_format()
    // prints them verbatim, so an out-of-memory rendered as "0:0:" with the
    // caret at column 0, out of contract. It came from assigning err.code
    // directly instead of recording through scan_fail(), which is the only
    // thing in the lexer that derives line and column.
    //
    // Found by review rather than by a test, because nothing else in the
    // suite injects an allocation failure into a parse.
    {
        const struct {
            unsigned    nth;
            const char *want;
            const char *msg;
        } row[] = {
            { 1, "@0 1:1: out of memory",
              "docface: a builder-stack OOM is positioned at the start of "
              "the document" },
            { 2, "@1 1:2: out of memory",
              "docface: a token-array OOM is positioned where the scan had "
              "reached, not at 0:0" },
        };
        const char *doc = "{\"a\":1}";
        size_t      i;

        for (i = 0; i < sizeof(row) / sizeof(row[0]); i++) {
            AxlJsonReader r;
            char          out[AXL_JSON_ERROR_BUF_MAX + 32];
            bool          parsed;

            /* Armed as late as possible and disarmed immediately: the counter
               is global and counts EVERY axl_malloc, so anything between
               these two lines would shift which allocation fails. */
            axl_mem_fail_next_alloc(row[i].nth);
            parsed = axl_json_parse(doc, axl_strlen(doc),
                                          AXL_JSON_STRICT, &r);
            axl_mem_fail_next_alloc(0);

            if (parsed) {
                axl_json_free(&r);
                test_check(false, row[i].msg);   /* the OOM never fired */
                continue;
            }
            test_check(err_render(&r, out, sizeof(out))
                           && axl_strcmp(out, row[i].want) == 0,
                       row[i].msg);
        }
    }
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
    test_array_sized_steal_clear();
    test_array_insert_prepend();
    test_array_data_and_element_size();
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
    test_json_decoded_len();
    test_json_write_double();
    test_json_get_object();
    test_json5_parse();
    test_json_unicode_escapes();
    test_json_nul_escape_union();
    test_json5_line_continuations();
    test_json_error_reporting();
    test_json_error_position();
    test_json_reader_consumed();
    test_json_error_argument_paths();
    test_json_accessor_utf8_integrity();
    test_json_load_file();
    test_json_build();
    test_json5_build();
    test_json_sinks();
    test_json_sources();
    test_json_value_mirror();
    test_json_object_iter();
    test_json_decode_string();
    test_json_writer_format();
    test_json_comment_depth0();
    test_json_comment_multiline();
    test_json_ensure_ascii();
    test_json_writer_utf8_mode();
    test_json_error_format();
    test_json_get_double();
    test_json_writer_roundtrip();
    test_json_encoding_boundary();
    test_json_line_endings();
    test_json_scanner();
    test_json_document_face();
    test_json_sort_keys();
    test_json_reject_duplicates();
    test_json_utf8_strict_read();
    test_json_utf8_repair_read();
    test_json_iter_aliasing();
    test_json_print();
    test_slist();
    test_list();
    test_queue();
    test_queue_deinit();
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
    test_radix_tree_null_value();
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
    test_compress_writer_ownership();
    test_compress_filters_refuse_a_transcoding_peer();
    test_compress_reader();
    test_checksum_type_length();

    axl_printf("\n--- OOM Injection ---\n");
    test_oom_containers();

    return test_print_results();
}

AXL_APP(test_data_main)
