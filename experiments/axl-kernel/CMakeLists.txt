# experiments/axl-kernel -- the kernel proof of concept: the context switch,
# the kernel, and the servers each linked over them.

axl_asm_object(axl_obj_experiments_axl_kernel_ctx_switch
    src/ctx-switch-${CMAKE_SYSTEM_PROCESSOR}.S)
axl_object(axl_obj_experiments_axl_kernel_kernel NONE src/kernel.c)
axl_includes_after(axl_obj_experiments_axl_kernel_kernel experiments/axl-kernel/include)
set(_kernel axl_obj_experiments_axl_kernel_ctx_switch axl_obj_experiments_axl_kernel_kernel)
foreach(_v AxlKernelPoc:kernel-poc axlk-echo-server:axlk-echo-server
           axlk-hwinfo-server:axlk-hwinfo-server axlk-bootconfig-server:axlk-bootconfig-server
           axlk-reqlog-server:axlk-reqlog-server)
    string(REPLACE ":" ";" _v ${_v})
    list(GET _v 0 _img)
    list(GET _v 1 _s)
    axl_obj_name(_n test/${_s}.c)
    axl_object(${_n} NONE test/${_s}.c)
    axl_includes_after(${_n} experiments/axl-kernel/include)
    axl_app_image(${_img} ${_kernel} ${_n})
endforeach()
