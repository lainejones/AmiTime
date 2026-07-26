# AmiTime - SNTP network time client for AmigaOS
# Build under WSL with the bebbo amiga-gcc toolchain on PATH.
#
# -m68000 so a single binary runs on any Amiga from a stock A500 upwards.
# Uses only the NDK's own AmiTCP/bsdsocket headers, so this repository is
# self-contained: no external include trees required.
#
#   make          build the program
#   make dist     build the release archives (.lha and .zip)
#   make clean    remove build products

VERSION = 1.1
CC      = m68k-amigaos-gcc
CFLAGS  = -m68000 -O2 -noixemul -Wall -fomit-frame-pointer

# Staged under dist/ rather than ./AmiTime because the build host's filesystem
# may be case-insensitive, where "AmiTime" and the "amitime" binary collide.
DISTDIR  = dist
DISTFILE = AmiTime-$(VERSION)

amitime: amitime.c tz.c tz.h
	$(CC) $(CFLAGS) -o $@ amitime.c tz.c -lamiga

# The POSIX TZ logic is plain C, so it is tested on the BUILD host - far
# quicker than pushing a binary to real hardware for every calendar edge case.
test:
	gcc -O2 -Wall -o /tmp/amitime_test_tz tz.c test_tz.c && /tmp/amitime_test_tz

# Release archives.  .lha first: that is what Amiga users and Aminet expect
# (and it is what unpacks natively on the machine itself); the .zip is for
# people grabbing it from GitHub on a modern desktop.  Both hold the same
# drawer, so the Install script finds AmiTime alongside itself either way.
dist: amitime
	rm -rf $(DISTDIR) $(DISTFILE).lha $(DISTFILE).zip
	mkdir -p $(DISTDIR)/AmiTime
	cp amitime $(DISTDIR)/AmiTime/AmiTime
	cp Install README.md LICENSE $(DISTDIR)/AmiTime/
	cd $(DISTDIR) && zip -qr ../$(DISTFILE).zip AmiTime
	@# .lha only if the host has an LHA that can CREATE archives.  Note that
	@# the common Linux "lha" (lhasa) is decompress-only, so this is usually
	@# skipped and the release .lha is made on an Amiga with the real LhA:
	@#     LhA a AmiTime-1.0.lha AmiTime
	@if lha 2>&1 | grep -q ' a ' ; then \
		cd $(DISTDIR) && lha -aq2 ../$(DISTFILE).lha AmiTime && \
		echo "built $(DISTFILE).lha and $(DISTFILE).zip" ; \
	else \
		echo "built $(DISTFILE).zip" ; \
		echo "NOTE: no archive-creating lha here (lhasa only extracts)." ; \
		echo "      Build the .lha on an Amiga:  LhA a $(DISTFILE).lha AmiTime" ; \
	fi
	rm -rf $(DISTDIR)

clean:
	rm -rf amitime $(DISTDIR) $(DISTFILE).lha $(DISTFILE).zip

.PHONY: dist clean test
