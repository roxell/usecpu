all: use_cpu

use_cpu: use_cpu.c
	$(CC) $< -o $@ -lpthread

clean:
	rm -f use_cpu
