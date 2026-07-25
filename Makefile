# AmiTime - SNTP network time client for AmigaOS
# Build under WSL with the bebbo amiga-gcc toolchain on PATH.
#
# -m68000 so one binary runs on every machine here (A1200 030, A4000 060).
# Uses a314bsd's AmiTCP-compatible socket headers - they are the standard
# bsdsocket API, so the result works on Roadshow/AmiTCP/Miami/a314bsd alike.

CC     = m68k-amigaos-gcc
CFLAGS = -m68000 -O2 -noixemul -Wall -fomit-frame-pointer \
         -I../a314bsd/include \
         -I../a314bsd/include/netinclude

amitime: amitime.c
	$(CC) $(CFLAGS) -o $@ $< -lamiga

clean:
	rm -f amitime
