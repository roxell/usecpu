all: use_cpu

LDLIBS := -lpthread

use_cpu: use_cpu.c
	$(CC) $< -o $@ $(LDLIBS)

clean:
	rm -f use_cpu
