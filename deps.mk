DEPS_LIB_NAME := rwMemory
DEPS_LIB_OBJS := lib/rwmem.o lib/rwmem_req.o lib/dmabuf.o lib/touch.o src/rwmem_sc.o
DEPS_LIB_INCS := lib src
DEPS_LIB_DEPS := KernCall Type_info
