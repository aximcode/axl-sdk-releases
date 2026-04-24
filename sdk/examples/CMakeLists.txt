cmake_minimum_required(VERSION 3.16)
project(axl-examples C)

# Locate the AXL SDK. If it's installed at a non-standard prefix,
# invoke cmake with -DCMAKE_PREFIX_PATH=/path/to/axl-sdk.
find_package(axl REQUIRED)

axl_add_app(hello hello.c)
axl_add_app(radix-demo radix-demo.c)
axl_add_app(ring-buf-demo ring-buf-demo.c)
axl_add_app(cancellable-demo cancellable-demo.c)
axl_add_app(event-demo event-demo.c)
axl_add_app(runtime-demo runtime-demo.c)
