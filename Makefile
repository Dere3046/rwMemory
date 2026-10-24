obj-m := rwmem.o

KDIR := $(KDIR)
MDIR := $(realpath $(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
ODIR := $(MDIR)/out/$(VER)

DEPS := KernCall Type_info
include $(MDIR)/mk/deps.mk

rwmem-y := src/rwmem_main.o src/rwmem_sc.o lib/rwmem.o lib/rwmem_req.o \
	lib/touch.o lib/dmabuf.o $(DEPS_OBJS_ALL)

ccflags-y += -std=gnu11
ccflags-y += -Wno-declaration-after-statement
ccflags-y += -Wno-unused-variable
ccflags-y += -Wno-unused-function
ccflags-y += -Wno-strict-prototypes
ccflags-y += -DCONFIG_TI_REMAP
ccflags-y += -DCONFIG_KERNSC_PATCH
ccflags-y += -DCONFIG_KERNSC_DISCOVER

ifdef RWMEM_MAPS_FINDVMA
ccflags-y += -DCONFIG_RWMEM_MAPS_FINDVMA
endif
ccflags-y += -I$(src)/lib
ccflags-y += $(addprefix -I$(src)/,$(DEPS_INCS_ALL))

$(info -- KDIR: $(KDIR))
$(info -- MDIR: $(MDIR))
$(info -- ODIR: $(ODIR))

all:
	mkdir -p $(ODIR)
	make -C $(KDIR) M=$(ODIR) src=$(MDIR) srcroot=$(MDIR) modules
clean:
	make -C $(KDIR) M=$(ODIR) src=$(MDIR) srcroot=$(MDIR) clean

$(obj)/%.o: $(src)/%.c $(recordmcount_source) FORCE
	$(call if_changed_rule,cc_o_c)
	$(call cmd,force_checksrc)

$(obj)/%.o: $(src)/%.S FORCE
	$(call if_changed_rule,as_o_S)
