/**
 * collections.c — AXL data structures: list, hash, array, queue.
 *
 * Demonstrates AxlSList, AxlList, AxlQueue, AxlHashTable, and AxlArray
 * in a single example. All are GLib-inspired with familiar APIs.
 *
 * Build with: axl-cc collections.c -o collections.efi
 */

#include <axl.h>

/* Compare for lists (data is the pointer value itself) */
static int
ptr_int_compare(const void *a, const void *b)
{
    intptr_t ia = (intptr_t)a;
    intptr_t ib = (intptr_t)b;
    return (ia > ib) - (ia < ib);
}

/* Compare for arrays (a, b point to stored int values) */
static int
val_int_compare(const void *a, const void *b)
{
    int ia = *(const int *)a;
    int ib = *(const int *)b;
    return (ia > ib) - (ia < ib);
}

int
main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    /* ---- Singly-linked list ---- */
    axl_printf("--- AxlSList (singly-linked) ---\n");
    AxlSList *sl = NULL;
    sl = axl_slist_prepend(sl, "cherry");
    sl = axl_slist_prepend(sl, "banana");
    sl = axl_slist_prepend(sl, "apple");
    for (AxlSList *n = sl; n; n = n->next) {
        axl_printf("  %s\n", (char *)n->data);
    }
    axl_printf("  length: %llu\n", (unsigned long long)axl_slist_length(sl));
    axl_slist_free(sl);

    /* ---- Doubly-linked list ---- */
    axl_printf("\n--- AxlList (doubly-linked, sorted) ---\n");
    AxlList *dl = NULL;
    dl = axl_list_insert_sorted(dl, (void *)30, ptr_int_compare);
    dl = axl_list_insert_sorted(dl, (void *)10, ptr_int_compare);
    dl = axl_list_insert_sorted(dl, (void *)20, ptr_int_compare);
    for (AxlList *n = dl; n; n = n->next) {
        axl_printf("  %lld\n", (long long)(intptr_t)n->data);
    }
    /* walk backwards from tail */
    AxlList *tail = axl_list_last(dl);
    axl_printf("  reverse: %lld %lld %lld\n",
               (long long)(intptr_t)tail->data,
               (long long)(intptr_t)tail->prev->data,
               (long long)(intptr_t)tail->prev->prev->data);
    axl_list_free(dl);

    /* ---- Queue (FIFO) ---- */
    axl_printf("\n--- AxlQueue (FIFO) ---\n");
    AxlQueue q = AXL_QUEUE_INIT;
    axl_queue_push_tail(&q, "first");
    axl_queue_push_tail(&q, "second");
    axl_queue_push_tail(&q, "third");
    while (!axl_queue_is_empty(&q)) {
        axl_printf("  dequeue: %s\n", (char *)axl_queue_pop_head(&q));
    }

    /* ---- Hash table ---- */
    axl_printf("\n--- AxlHashTable (string keys) ---\n");
    AXL_AUTOPTR(AxlHashTable) h = axl_hash_table_new_str();
    axl_hash_table_insert(h, "name", "AXL");
    axl_hash_table_insert(h, "type", "UEFI library");
    axl_hash_table_insert(h, "style", "GLib-inspired");
    axl_printf("  name: %s\n", (char *)axl_hash_table_lookup(h, "name"));
    axl_printf("  type: %s\n", (char *)axl_hash_table_lookup(h, "type"));
    axl_printf("  entries: %llu\n", (unsigned long long)axl_hash_table_size(h));

    /* ---- Dynamic array ---- */
    axl_printf("\n--- AxlArray (dynamic, sorted) ---\n");
    AXL_AUTOPTR(AxlArray) a = axl_array_new(sizeof(int));
    int vals[] = {50, 20, 40, 10, 30};
    for (int i = 0; i < 5; i++) {
        axl_array_append(a, &vals[i]);
    }
    axl_array_sort(a, val_int_compare);
    for (size_t i = 0; i < axl_array_len(a); i++) {
        axl_printf("  %d\n", *(int *)axl_array_get(a, i));
    }

    return 0;
}
