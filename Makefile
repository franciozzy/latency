CC=gcc
CFLAGS=-Wall -O3 -s
TARGET=latency

all: $(TARGET)

%: %.c
	$(CC) -o $@ $< $(CFLAGS)

clean:
	rm -f $(TARGET)
