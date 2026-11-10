# src/cxxrt -- the C library's porting objects and the C++ runtime glue.
axl_objects(SRC axl-cxxrt-alloc.c axl-cxxrt-eh.c axl-cxxrt-stubs.c)

# The C++ runtime glue: exceptions and RTTI ON (it implements them).
foreach(_src axl-cxxrt-nothrow.cpp axl-cxxrt-terminate.cpp)
    axl_obj_name(_n ${_src})
    axl_object(${_n} CXX ${_src})
    set_target_properties(${_n} PROPERTIES AXL_CXX_EH ON)
endforeach()
