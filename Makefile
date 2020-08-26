all: use_cpu

LDLIBS := -lpthread
ifneq ($(STATIC),)
	LDFLAGS := -static
endif

use_cpu: use_cpu.c
	$(CROSS_COMPILE)gcc $< -o $@ $(LDLIBS) $(LDFLAGS)

clean:
	rm -f use_cpu
