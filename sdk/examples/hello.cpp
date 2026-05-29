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

// Static initializer — proves .init_array runs before main().
const char *const kDefaultName = "world";

} // namespace

int
main(int argc, char **argv)
{
    const char *name = (argc < 2) ? kDefaultName : argv[1];

    Greeter *g = new Greeter();
    g->greet(name);
    delete g;

    return 0;
}
