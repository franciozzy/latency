.PHONY: all
all: latency

%: %.c
	gcc -o $@ $< -Wall -O3 -s
