CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror
PJSIP_CFLAGS ?= $(shell pkg-config --static --cflags libpjproject 2>/dev/null)
PJSIP_LIBS ?= $(shell pkg-config --static --libs libpjproject 2>/dev/null)

TARGETS := decode encode

# If PJSIP is available, also build sip
ifneq ($(strip $(PJSIP_LIBS)),)
TARGETS += sip
endif

.PHONY: all clean test

all: $(TARGETS)

decode: decode.c
	$(CC) $(CFLAGS) decode.c -lm -o $@

encode: encode.c
	$(CC) $(CFLAGS) encode.c -lm -o $@

sip: sip.c
	$(CC) $(CFLAGS) $(PJSIP_CFLAGS) sip.c $(PJSIP_LIBS) -pthread -o $@

test: all
	./tests/run_tests.sh

clean:
	rm -f decode encode sip
