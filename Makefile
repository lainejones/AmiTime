# AmiTime - SNTP network time client for AmigaOS
# Build under WSL with the bebbo amiga-gcc toolchain on PATH.
#
# -m68000 so a single binary runs on any Amiga from a stock A500 upwards.
# Uses only the NDK's own AmiTCP/bsdsocket headers, so this repository is
# self-contained: no external include trees required.

CC     = m68k-amigaos-gcc
CFLAGS = -m68000 -O2 -noixemul -Wall -fomit-frame-pointer

amitime: amitime.c
	$(CC) $(CFLAGS) -o $@ $< -lamiga

clean:
	rm -f amitime
