/**
 * containers.cpp — the standard containers, under UEFI.
 *
 * Build with: axl-c++ containers.cpp -o containers.efi
 *
 * No flag. This used to need `--hosted`, because libstdc++ refuses
 * <vector>/<string>/<map>/<unordered_map> under -ffreestanding at
 * bits/requires_hosted.h — and that flag, not exceptions and not the
 * heap, was the whole gate. AXL no longer compiles C++ freestanding,
 * so the containers are simply available.
 *
 * Everything still allocates through axl_malloc, so AxlMem's leak
 * tracking and debug fill pattern keep working unchanged.
 *
 * ONE thing is different from a hosted program: allocation failure
 * HALTS. `operator new` may not return NULL here, because libstdc++
 * hands its result to the container without a null check. The second
 * half of this example shows what to do on a path that cannot afford
 * that.
 */

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include <axl.h>
#include <axl/axl-arena-allocator.hpp>

static void
standard_containers(void)
{
    std::vector<std::string> names{"pear", "apple", "fig"};
    std::sort(names.begin(), names.end());

    axl_print("sorted:");
    for (const auto &n : names) {
        axl_printf(" %s", n.c_str());
    }
    axl_print("\r\n");

    std::map<std::string, int> sizes;
    for (const auto &n : names) {
        sizes[n] = (int) n.size();
    }
    axl_printf("fig is %d chars, %u entries\r\n",
               sizes["fig"], (unsigned) sizes.size());
}

/* A path that must degrade rather than halt when memory runs short.
 *
 * The arena's capacity is fixed at creation, so once it exists and is
 * big enough, allocation from it cannot fail. That turns "out of
 * memory" from an event scattered through the container's internals
 * into one condition checked here, where returning false is an option.
 *
 * deallocate() is a no-op on an arena, so reserve() up front is a
 * correctness concern and not a performance note: growing a vector to
 * capacity N without reserving consumes about 2N elements' worth of
 * arena and gives none of it back. bytes_for() answers for ONE
 * allocation.
 */
static bool
bounded_work(size_t n_items)
{
    using ids   = std::vector<uint32_t, axl::arena_allocator<uint32_t>>;
    using alloc = ids::allocator_type;

    AxlArena *arena = axl_arena_new(64 * 1024);
    if (arena == NULL) {
        return false;                       /* caller degrades */
    }
    if (axl_arena_remaining(arena) < alloc::bytes_for(n_items)) {
        axl_arena_free(arena);
        return false;                       /* caller degrades */
    }

    {
        ids v{alloc(arena)};
        v.reserve(n_items);                 /* cannot fail from here */
        for (size_t i = 0; i < n_items; i++) {
            v.push_back((uint32_t) (i * i));
        }
        axl_printf("arena-backed: %u ids, %zu bytes left\r\n",
                   (unsigned) v.size(), axl_arena_remaining(arena));
    }
    /* The container is destroyed before the arena: element destructors
     * run over arena memory, so freeing first would be a use-after-free
     * for any element type that has one. */
    axl_arena_free(arena);
    return true;
}

int
main(void)
{
    standard_containers();

    if (!bounded_work(4096)) {
        axl_print("arena-backed: not enough memory, degraded\r\n");
    }
    return 0;
}
