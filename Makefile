obj-m := rwmem.o

rwmem-y := src/rwmem_main.o src/rwmem_sc.o lib/rwmem.o lib/touch.o \
	deps/Kerncall/lib/sc.o deps/Kerncall/lib/sc_slide.o \
	deps/hidemod/lib/hidemod.o \
	deps/type_info/lib/port.o deps/type_info/lib/slide.o \
	deps/type_info/lib/btf.o deps/type_info/lib/query.o \
	deps/type_info/lib/reg.o deps/type_info/lib/lib.o \
	deps/type_info/lib/anchor.o deps/type_info/lib/dwarf.o \
	deps/type_info/kallrecon/lib/core.o \
	deps/type_info/kallrecon/lib/anchor.o

ccflags-y += -std=gnu11
ccflags-y += -Wno-declaration-after-statement
ccflags-y += -Wno-unused-variable
ccflags-y += -Wno-unused-function
ccflags-y += -Wno-strict-prototypes
ccflags-y += -DCONFIG_TI_REMAP
ccflags-y += -DCONFIG_KERNSC_PATCH
ccflags-y += -DCONFIG_KERNSC_DISCOVER

ifdef RWMEM_HIDE
ccflags-y += -DCONFIG_RWMEM_HIDE
endif

ifdef RWMEM_MAPS_FINDVMA
ccflags-y += -DCONFIG_RWMEM_MAPS_FINDVMA
endif
ccflags-y += -I$(src)/lib
ccflags-y += -I$(src)/deps/Kerncall/lib
ccflags-y += -I$(src)/deps/hidemod/lib
ccflags-y += -I$(src)/deps/type_info/lib
ccflags-y += -I$(src)/deps/type_info/kallrecon/lib

KDIR := $(KDIR)
MDIR := $(realpath $(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
ODIR := $(MDIR)/out/$(VER)

$(info -- KDIR: $(KDIR))
$(info -- MDIR: $(MDIR))
$(info -- ODIR: $(ODIR))

all:
	make -C $(KDIR) M=$(ODIR) src=$(MDIR) modules
clean:
	make -C $(KDIR) M=$(ODIR) src=$(MDIR) clean

$(obj)/%.o: $(src)/%.c $(recordmcount_source) FORCE
	$(call if_changed_rule,cc_o_c)
	$(call cmd,force_checksrc)

$(obj)/%.o: $(src)/%.S FORCE
	$(call if_changed_rule,as_o_S)
