CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror
PJSIP_CFLAGS ?= $(shell pkg-config --static --cflags libpjproject 2>/dev/null)
PJSIP_LIBS ?= $(shell pkg-config --static --libs libpjproject 2>/dev/null)

TARGETS := decode encode

# If PJSIP is available, build both SIP responders.
ifneq ($(strip $(PJSIP_LIBS)),)
TARGETS += sip-onetime sip-dialog
endif

.PHONY: all clean test

all: $(TARGETS)

decode: decode.c
	$(CC) $(CFLAGS) decode.c -lm -o $@

encode: encode.c
	$(CC) $(CFLAGS) encode.c -lm -o $@

sip-onetime: sip-onetime.c
	$(CC) $(CFLAGS) $(PJSIP_CFLAGS) sip-onetime.c $(PJSIP_LIBS) -pthread -o $@

sip-dialog: sip-dialog.c decode.c decode.h
	$(CC) $(CFLAGS) $(PJSIP_CFLAGS) -DDECODE_LIBRARY_ONLY sip-dialog.c decode.c -lm $(PJSIP_LIBS) -lutil -pthread -o $@

# Keep the original deployment target used by the Pterodactyl egg.
ifneq ($(strip $(PJSIP_LIBS)),)
.PHONY: sip
sip: sip-onetime
	cp sip-onetime sip
endif

test: all
	./tests/run_tests.sh

clean:
	rm -f decode encode sip sip-onetime sip-dialog
