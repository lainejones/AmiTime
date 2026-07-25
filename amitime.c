/*
 * AmiTime - SNTP network time client for AmigaOS
 *
 * Copyright (c) 2026 Laine Jones
 * SPDX-License-Identifier: MIT
 *
 * Sets the Amiga system clock (and optionally the battery-backed clock) from
 * a network time server, for machines whose RTC is missing, dead, or simply
 * drifting.  Speaks SNTP (RFC 4330) over UDP/123: one 48-byte request, one
 * reply, using the reply's Transmit Timestamp.
 *
 * Works on ANY TCP/IP stack that provides bsdsocket.library - Roadshow,
 * AmiTCP, Miami, and also a314bsd (where the sockets physically live on a
 * Raspberry Pi).  Nothing here is specific to any hardware.  Built for
 * plain 68000 / AmigaOS 2.0+.
 *
 * Usage:  AmiTime [HOST <server>] [TZ <hours>] [PORT <n>] [TIMEOUT <secs>]
 *                 [SAVE] [SHOW] [QUIET]
 */

#include <exec/types.h>
#include <exec/io.h>
#include <exec/ports.h>
#include <devices/timer.h>
#include <dos/dos.h>
#include <dos/datetime.h>
#include <libraries/locale.h>
#include <resources/battclock.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/locale.h>
#include <proto/battclock.h>
#include <proto/bsdsocket.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>

#include <stdarg.h>
#include <string.h>
#include <stdio.h>

/* Amiga version string, found by the AmigaDOS "Version" command.  It has to
 * be REFERENCED by real code or the linker discards it (a bare
 * __attribute__((used)) const array was not enough here), so the VERSION
 * switch below prints it - which makes it useful rather than just present. */
#define AMITIME_VERSION "AmiTime 1.0 (25.7.2026)"
static const char verstag[] = "$VER: " AMITIME_VERSION;

/* Library bases that the proto-header inlines expect us to provide. */
struct Library     *SocketBase    = NULL;
struct LocaleBase  *LocaleBase    = NULL;
struct Library     *BattClockBase = NULL;

/* Seconds between the NTP epoch (1900-01-01) and the AmigaDOS epoch
 * (1978-01-01): 28489 days (78 years + 19 leap days) * 86400. */
#define NTP_TO_AMIGA_EPOCH 2461449600UL

#define SNTP_PACKET_SIZE 48
#define SNTP_XMIT_OFFSET 40      /* transmit timestamp, seconds part */
#define SNTP_DEFAULT_HOST "pool.ntp.org"
#define SNTP_DEFAULT_PORT 123

static BOOL quiet = FALSE;

static void say(CONST_STRPTR fmt, ...);   /* forward; trivial printf wrapper */

/* ---- clock ------------------------------------------------------------- */

/* Set the AmigaOS system time via timer.device. */
static BOOL set_system_time(ULONG secs, ULONG micro)
{
    struct MsgPort     *port;
    struct timerequest *req;
    BOOL                ok = FALSE;

    port = CreateMsgPort();
    if (!port) return FALSE;
    req = (struct timerequest *)CreateIORequest(port, sizeof(*req));
    if (!req) { DeleteMsgPort(port); return FALSE; }

    if (OpenDevice((STRPTR)TIMERNAME, UNIT_VBLANK,
                   (struct IORequest *)req, 0) == 0) {
        req->tr_node.io_Command = TR_SETSYSTIME;
        req->tr_time.tv_secs  = secs;
        req->tr_time.tv_micro = micro;
        if (DoIO((struct IORequest *)req) == 0) ok = TRUE;
        CloseDevice((struct IORequest *)req);
    }
    DeleteIORequest((struct IORequest *)req);
    DeleteMsgPort(port);
    return ok;
}

/* Write the battery-backed clock (same job as "SetClock save"), so the time
 * survives a power cycle on machines that have a working RTC + battery. */
static BOOL save_battery_clock(ULONG secs)
{
    BattClockBase = (struct Library *)OpenResource((STRPTR)BATTCLOCKNAME);
    if (!BattClockBase) return FALSE;
    WriteBattClock(secs);
    return TRUE;
}

/* ---- timezone ----------------------------------------------------------
 * AmigaOS keeps the system clock in LOCAL time, so the UTC offset has to be
 * applied before setting it.  Prefer the user's Locale preferences
 * (loc_GMTOffset, minutes WEST of GMT) so the tool is correct out of the box
 * on any machine; a TZ argument or ENV:AMITIME_TZ overrides it. */
static BOOL locale_tz_minutes(LONG *minutes_east)
{
    struct Locale *loc;

    LocaleBase = (struct LocaleBase *)OpenLibrary((STRPTR)"locale.library", 38);
    if (!LocaleBase) return FALSE;

    loc = OpenLocale(NULL);
    if (!loc) {
        CloseLibrary((struct Library *)LocaleBase);
        LocaleBase = NULL;
        return FALSE;
    }

    /* loc_GMTOffset is minutes WEST of Greenwich; we want minutes EAST. */
    *minutes_east = -loc->loc_GMTOffset;
    CloseLocale(loc);
    CloseLibrary((struct Library *)LocaleBase);
    LocaleBase = NULL;
    return TRUE;
}

/* ---- SNTP ---------------------------------------------------------------- */

/* Query `host`.  On success stores the server's transmit timestamp (seconds
 * and fraction since 1900) and returns TRUE. */
static BOOL sntp_query(STRPTR host, UWORD port, LONG timeout_secs,
                       ULONG *ntp_secs, ULONG *ntp_frac)
{
    struct hostent *he;
    volatile UBYTE  sabuf[16];        /* raw sockaddr_in - see note below */
    UBYTE           pkt[SNTP_PACKET_SIZE];
    LONG            fd, n, i;
    ULONG           addr;
    BOOL            ok = FALSE;

    he = gethostbyname(host);
    if (!he || !he->h_addr_list || !he->h_addr_list[0]) {
        say((CONST_STRPTR)"AmiTime: cannot resolve '%s'\n", host);
        return FALSE;
    }
    addr = *(ULONG *)he->h_addr_list[0];

    /* IPPROTO_UDP explicitly, NOT 0: Roadshow will happily create a
     * protocol-0 datagram socket and then refuse to connect() it. */
    fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        say((CONST_STRPTR)"AmiTime: cannot create UDP socket (errno %ld)\n",
            (LONG)Errno());
        return FALSE;
    }

    /* Build the sockaddr as VOLATILE BYTES.  Both the `volatile` and the
     * byte-at-a-time writes are load-bearing - please do not "tidy" this into
     * struct field assignments.  With -O2 this compiler merges the adjacent
     * small stores and drops the sin_family half, yielding 0000 007b instead
     * of 0002 007b, and the stack then rejects the socket with EAFNOSUPPORT.
     * Layout: family(2) port(2) addr(4), big-endian throughout on 68k.
     * Byte 0 (sin_len in the BSD 4.4 layout) is left 0, which is what the
     * stacks tested here accept. */
    for (i = 0; i < 16; i++) sabuf[i] = 0;
    sabuf[1] = AF_INET;
    sabuf[2] = (UBYTE)(port >> 8);  sabuf[3] = (UBYTE)port;
    sabuf[4] = (UBYTE)(addr >> 24); sabuf[5] = (UBYTE)(addr >> 16);
    sabuf[6] = (UBYTE)(addr >> 8);  sabuf[7] = (UBYTE)addr;

    /* Connected UDP: the stack then filters replies from other hosts for us,
     * and we can use plain send/recv. */
    if (connect(fd, (struct sockaddr *)sabuf, 16) < 0) {
        say((CONST_STRPTR)"AmiTime: cannot reach the server (errno %ld)\n",
            (LONG)Errno());
        CloseSocket(fd);
        return FALSE;
    }

    /* Three attempts: one lost datagram shouldn't leave the clock unset, and
     * at boot the interface may only just have come up. */
    for (i = 0; i < 3 && !ok; i++) {
        fd_set         rfds;
        struct timeval tv;

        memset(pkt, 0, sizeof(pkt));
        pkt[0] = 0x1B;          /* LI=0, VN=3, Mode=3 (client) */

        if (send(fd, pkt, sizeof(pkt), 0) < 0) {
            say((CONST_STRPTR)"AmiTime: send failed (errno %ld)\n", (LONG)Errno());
            break;
        }

        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        tv.tv_secs  = timeout_secs;
        tv.tv_micro = 0;
        if (WaitSelect(fd + 1, &rfds, NULL, NULL, &tv, NULL) <= 0) {
            say((CONST_STRPTR)"AmiTime: no reply (attempt %ld of 3)\n", (LONG)(i + 1));
            continue;
        }

        n = recv(fd, pkt, sizeof(pkt), 0);
        if (n < SNTP_PACKET_SIZE) continue;

        /* Mode 4 = server reply; stratum 0 is a "kiss of death", not a time. */
        if ((pkt[0] & 0x07) != 4 || pkt[1] == 0) {
            say((CONST_STRPTR)"AmiTime: the server declined the request\n");
            continue;
        }

        *ntp_secs = ((ULONG)pkt[SNTP_XMIT_OFFSET + 0] << 24) |
                    ((ULONG)pkt[SNTP_XMIT_OFFSET + 1] << 16) |
                    ((ULONG)pkt[SNTP_XMIT_OFFSET + 2] <<  8) |
                     (ULONG)pkt[SNTP_XMIT_OFFSET + 3];
        *ntp_frac = ((ULONG)pkt[SNTP_XMIT_OFFSET + 4] << 24) |
                    ((ULONG)pkt[SNTP_XMIT_OFFSET + 5] << 16) |
                    ((ULONG)pkt[SNTP_XMIT_OFFSET + 6] <<  8) |
                     (ULONG)pkt[SNTP_XMIT_OFFSET + 7];
        if (*ntp_secs) ok = TRUE;
    }

    CloseSocket(fd);
    return ok;
}

/* ---- helpers ------------------------------------------------------------- */

static void say(CONST_STRPTR fmt, ...)
{
    va_list ap;
    if (quiet) return;
    va_start(ap, fmt);
    vprintf((const char *)fmt, ap);
    va_end(ap);
}

static void print_time(CONST_STRPTR label, ULONG secs)
{
    struct DateTime dt;
    char ds[LEN_DATSTRING], ts[LEN_DATSTRING];

    memset(&dt, 0, sizeof(dt));
    dt.dat_Stamp.ds_Days   = (LONG)(secs / 86400);
    dt.dat_Stamp.ds_Minute = (LONG)((secs % 86400) / 60);
    dt.dat_Stamp.ds_Tick   = (LONG)((secs % 60) * TICKS_PER_SECOND);
    dt.dat_Format  = FORMAT_DOS;
    dt.dat_StrDate = (STRPTR)ds;
    dt.dat_StrTime = (STRPTR)ts;
    if (DateToStr(&dt))
        say((CONST_STRPTR)"%s %s %s\n", label, (STRPTR)ds, (STRPTR)ts);
}

static BOOL get_env_str(CONST_STRPTR name, STRPTR buf, LONG size)
{
    LONG n = GetVar((STRPTR)name, buf, size - 1, 0);
    if (n <= 0) return FALSE;
    buf[n] = 0;
    return buf[0] != 0;
}

static LONG parse_int(CONST_STRPTR s)
{
    LONG v = 0, sign = 1;
    if (*s == '-') { sign = -1; s++; } else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return v * sign;
}

/* ---- main ---------------------------------------------------------------- */

int main(void)
{
    struct RDArgs *rdargs;
    LONG   rda[8];
    UBYTE  hostbuf[128], envbuf[64];
    STRPTR host     = (STRPTR)SNTP_DEFAULT_HOST;
    LONG   port     = SNTP_DEFAULT_PORT;
    LONG   timeout  = 5;
    LONG   tz_mins  = 0;
    BOOL   tz_set   = FALSE;
    BOOL   showonly = FALSE, savertc = FALSE, showver = FALSE;
    ULONG  ntp_secs = 0, ntp_frac = 0, amiga_secs;
    LONG   i;
    int    rc = RETURN_FAIL;

    /* Defaults from ENV: so User-Startup can be a bare "AmiTime". */
    if (get_env_str((CONST_STRPTR)"AMITIME_HOST", (STRPTR)hostbuf, sizeof(hostbuf)))
        host = (STRPTR)hostbuf;
    if (get_env_str((CONST_STRPTR)"AMITIME_TZ", (STRPTR)envbuf, sizeof(envbuf))) {
        tz_mins = parse_int((CONST_STRPTR)envbuf) * 60;
        tz_set  = TRUE;
    }

    for (i = 0; i < 8; i++) rda[i] = 0;
    rdargs = ReadArgs((STRPTR)"HOST/K,TZ/K/N,PORT/K/N,TIMEOUT/K/N,SAVE/S,SHOW/S,QUIET/S,VERSION/S",
                      rda, NULL);
    if (rdargs) {
        if (rda[0]) {
            STRPTR s = (STRPTR)rda[0];
            for (i = 0; i < (LONG)sizeof(hostbuf) - 1 && s[i]; i++) hostbuf[i] = s[i];
            hostbuf[i] = 0;
            host = (STRPTR)hostbuf;
        }
        if (rda[1]) { tz_mins = (*(LONG *)rda[1]) * 60; tz_set = TRUE; }
        if (rda[2]) port     = *(LONG *)rda[2];
        if (rda[3]) timeout  = *(LONG *)rda[3];
        if (rda[4]) savertc  = TRUE;
        if (rda[5]) showonly = TRUE;
        if (rda[6]) quiet    = TRUE;
        if (rda[7]) showver  = TRUE;
        FreeArgs(rdargs);
    }
    if (timeout < 1) timeout = 1;

    /* Answer VERSION before doing anything else, so it prints one clean line. */
    if (showver) {
        printf("%s\n", verstag + 6);   /* skip the "$VER: " marker */
        return RETURN_OK;
    }

    /* No explicit offset given?  Fall back to the machine's Locale
     * preferences, and say so - a surprising number of systems never have
     * Locale's GMT offset set, and silently using a wrong offset is far worse
     * than telling the user where the number came from. */
    if (!tz_set) {
        LONG mins;
        if (locale_tz_minutes(&mins)) {
            tz_mins = mins;
            say((CONST_STRPTR)"AmiTime: using Locale offset (%ld minutes from UTC)\n",
                (LONG)tz_mins);
        } else {
            say((CONST_STRPTR)"AmiTime: no Locale offset available - assuming UTC\n");
        }
    }

    SocketBase = OpenLibrary((STRPTR)"bsdsocket.library", 4);
    if (!SocketBase) {
        say((CONST_STRPTR)"AmiTime: no bsdsocket.library - is the TCP/IP stack running?\n");
        return RETURN_FAIL;
    }

    if (sntp_query(host, (UWORD)port, timeout, &ntp_secs, &ntp_frac)) {
        if (ntp_secs < NTP_TO_AMIGA_EPOCH) {
            say((CONST_STRPTR)"AmiTime: the server's time predates 1978 - ignoring\n");
        } else {
            amiga_secs = ntp_secs - NTP_TO_AMIGA_EPOCH + (ULONG)(tz_mins * 60);
            print_time((CONST_STRPTR)"AmiTime:", amiga_secs);

            if (showonly) {
                rc = RETURN_OK;
            } else {
                /* Fraction is in units of 1/2^32 s; scale to microseconds
                 * without needing a 64-bit multiply. */
                ULONG micro = (ULONG)(((ntp_frac >> 12) / 4295) % 1000000);
                if (set_system_time(amiga_secs, micro)) {
                    rc = RETURN_OK;
                    if (savertc) {
                        if (save_battery_clock(amiga_secs))
                            say((CONST_STRPTR)"AmiTime: system and battery clock set\n");
                        else
                            say((CONST_STRPTR)"AmiTime: system clock set (no battery clock present)\n");
                    } else {
                        say((CONST_STRPTR)"AmiTime: system clock set\n");
                    }
                } else {
                    say((CONST_STRPTR)"AmiTime: failed to set the system clock\n");
                }
            }
        }
    }

    CloseLibrary(SocketBase);
    return rc;
}
