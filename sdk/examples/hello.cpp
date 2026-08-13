/**
 * hello.cpp — minimal AXL SDK C++ example.
 *
 * Exercises the C++ runtime that ships in libaxl-cxx.a: heap
 * allocation (operator new/delete), a virtual call, and a static
 * initializer that runs before main() via the .init_array walker.
 * The AXL public API is plain C and is called directly.
 *
 * Build with: axl-c++ hello.cpp -o hello.efi
 */

#include <axl.h>

namespace {

class Greeter {
public:
    virtual ~Greeter() = default;
    virtual void greet(const char *name) const {
        axl_printf("Hello, %s!\n", name);
    }
};

// A real dynamic initializer: the constructor runs before main() via the
// .init_array walker. This used to be `const char *const kDefaultName =
// "world";` under the same comment -- a CONSTANT initializer, which emits no
// constructor and no .init_array entry, so it proved nothing. It was still
// claiming proof while --gc-sections was collecting .init_array outright and
// no global constructor in any image ran at all.
struct DefaultName {
    const char *value;
    DefaultName() : value("world") {}
};
const DefaultName kDefault;

} // namespace

int
main(int argc, char **argv)
{
    const char *name = (argc < 2) ? kDefault.value : argv[1];

    Greeter *g = new Greeter();
    g->greet(name);
    delete g;

    return 0;
}
