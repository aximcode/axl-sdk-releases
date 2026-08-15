cmake_minimum_required(VERSION 3.16)
# C and CXX. The C++ examples were unreachable from this build for as long as
# it said `C` alone -- check-examples compiled them, so they were not DEAD, but
# the build system a consumer copies had no C++ path at all.
project(axl-examples C CXX)

# Locate the AXL SDK. If it's installed at a non-standard prefix,
# invoke cmake with -DCMAKE_PREFIX_PATH=/path/to/axl-sdk.
find_package(axl REQUIRED)

axl_add_app(hello hello.c)
axl_add_app(radix-demo radix-demo.c)
axl_add_app(ring-buf-demo ring-buf-demo.c)
axl_add_app(cancellable-demo cancellable-demo.c)
axl_add_app(event-demo event-demo.c)
axl_add_app(runtime-demo runtime-demo.c)

# C++ examples. hello.cpp, cxx-errors.cpp and cxx-streams.cpp are
# freestanding; containers.cpp needs HOSTED, which is what makes
# std::vector / std::string / std::map available (libstdc++ refuses them
# under -ffreestanding).
axl_add_app(hello-cxx hello.cpp)
axl_add_app(cxx-errors cxx-errors.cpp)
axl_add_app(cxx-streams cxx-streams.cpp)
axl_add_app(containers containers.cpp HOSTED)
