CFLAGS ?= -O2 -g -Wall -Wextra -Wno-sign-compare
PREFIX ?= /usr/local

peeryeet-signal: signal.c
	$(CC) $(CFLAGS) -o $@ $<

install: peeryeet-signal
	install -m 0755 peeryeet-signal $(DESTDIR)$(PREFIX)/bin/peeryeet-signal

clean:
	rm -f peeryeet-signal

.PHONY: install clean
