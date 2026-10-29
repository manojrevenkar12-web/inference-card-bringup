# Out-of-tree build: make KDIR=/path/to/linux (defaults to the running kernel's headers)
KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

# sparse (C=2 checks every file) and the kernel's coding-style checker
# sparse (C=2 checks every file) and the kernel's coding-style checker in strict mode.
# EMBEDDED_FILENAME: lacc.h includes "uapi/lacc.h", which checkpatch reads as the file naming itself.
check:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
	$(MAKE) -C $(KDIR) M=$(CURDIR) C=2 W=1 modules 2>&1 | tee check.log
	@! grep -E "warning:|error:" check.log
	$(KDIR)/scripts/checkpatch.pl --no-tree --strict --ignore=EMBEDDED_FILENAME --terse \
		-f lacc_main.c -f lacc_mem.c -f lacc_i2c.c -f lacc_spi.c -f lacc_sysfs.c -f lacc.h \
		-f lacc_trace.h | tee -a check.log
	@! grep -E ": (ERROR|WARNING|CHECK):" check.log
	@echo "driver check: sparse, W=1 and checkpatch --strict are clean"

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
	rm -f check.log
