/* amitime.c - SNTP network time client for AmigaOS
 *
 * Sets the Amiga system clock from a network time server.  Written for
 * machines whose RTC battery is dead or absent (both the A4000 and the A1200
 * here boot to 1-Jan-78), so the clock is correct from the first boot without
 * any hardware repair.
 *
 * Portable across TCP/IP stacks by design: it uses only plain bsdsocket.library
 * calls (socket / sendto / WaitSelect / recvfrom / gethostbyname), so the SAME
 * binary runs on Roadshow, AmiTCP, Miami *and* on a314bsd (where the sockets
 * actually live on a Raspberry Pi).  Nothing here is A314- or ZZ9000-specific.
 *
 * Protocol: SNTP (RFC 4330) over UDP/123 - one 48-byte request, one reply; we
 * use the reply's Transmit Timestamp.  That is all a client needs.
 *
 * Usage:
 *   AmiTime [HOST <server>] [TZ <hours>] [PORT <n>] [TIMEOUT <secs>] [SHOW]
 *     HOST     time server (default: ENV:AMITIME_HOST, else pool.ntp.org)
 *     TZ       hours offset from UTC, may be negative (default: ENV:AMITIME_TZ, else 0)
 *     PORT     server port (default 123)
 *     TIMEOUT  seconds to wait per attempt (default 5, 3 attempts)
 *     SHOW     query and print the time but do NOT set the clock
 *
 * Typical use in S:User-Startup (after the network is up):
 *     C:AmiTime HOST 192.168.50.33 TZ -6 >NIL:
 *
 * Build (WSL/bebbo): make
 */

#include <exec/types.h>
#include <exec/io.h>
#include <exec/ports.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/rdargs.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>
#include <stdio.h>

#include <netinclude/sys/socket.h>
#include <netinclude/netinet/in.h>
#include <netinclude/netdb.h>
#include <netinclude/sys/select.h>
#include <inline/bsdsocket.h>

struct Library *SocketBase = NULL;

/* devices/timer.h arrives via proto/dos.h -> dos/dosextens.h.  No struct
 * clash to work around: this NDK's `struct timeval` uses anonymous unions, so
 * tv_sec/tv_usec (BSD spelling, what the socket code uses) and tv_secs/
 * tv_micro (AmigaOS spelling) are the same two fields. */

/* Seconds between the NTP epoch (1900-01-01) and the AmigaDOS epoch
 * (1978-01-01): 28489 days (78 years, 19 leap days) * 86400. */
#define NTP_TO_AMIGA_EPOCH 2461449600UL

#define SNTP_PACKET_SIZE 48
#define SNTP_XMIT_OFFSET 40      /* transmit timestamp, seconds part */

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

/* Read an ENV: variable into buf.  Returns TRUE if it existed and was
 * non-empty.  Used so HOST/TZ can be configured once instead of being baked
 * into every User-Startup line. */
static BOOL get_env(CONST_STRPTR name, STRPTR buf, LONG size)
{
    LONG n = GetVar((STRPTR)name, buf, size - 1, 0);
    if (n <= 0) return FALSE;
    buf[n] = 0;
    return buf[0] != 0;
}

/* Query `host`.  On success stores the server's transmit timestamp (seconds
 * since 1900) and returns TRUE. */
static BOOL sntp_query(STRPTR host, UWORD port, LONG timeout_secs,
                       ULONG *ntp_secs, ULONG *ntp_frac)
{
    struct hostent    *he;
    volatile UBYTE     sabuf[16];     /* volatile: see comment below */
    UBYTE              pkt[SNTP_PACKET_SIZE];
    LONG               fd, n, i;
    ULONG              addr;
    BOOL               ok = FALSE;

    he = gethostbyname(host);
    if (!he || !he->h_addr_list || !he->h_addr_list[0]) {
        printf("AmiTime: cannot resolve '%s'\n", (char *)host);
        return FALSE;
    }
    addr = *(ULONG *)he->h_addr_list[0];

    /* IPPROTO_UDP explicitly, NOT 0: measured via the DIAG switch, Roadshow
     * creates a socket for protocol 0 but then fails connect() on it with
     * EAFNOSUPPORT, while the same sequence with IPPROTO_UDP succeeds. */
    fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        printf("AmiTime: cannot create UDP socket (errno %ld)\n", (long)Errno());
        return FALSE;
    }

    /* Build the sockaddr as VOLATILE BYTES.  Both the `volatile` and the
     * byte-at-a-time writes are load-bearing - do not "tidy" this back into
     * `struct sockaddr_in` field assignments.
     *
     * Why: with -O2 this m68k gcc merges the adjacent small stores into one
     * 32-bit store and loses the sin_family half, producing a sockaddr of
     * 0000 007b (family 0, port 123) instead of 0002 007b.  Roadshow then
     * rejects the socket with EAFNOSUPPORT (errno 47).  It reproduced through
     * struct fields AND through a plain UBYTE array, and only when the port
     * came from a variable - with a literal port the identical source was
     * fine, which is what made this so slow to pin down.  `volatile` forbids
     * the merge; the dump then read 0002 007b and connect() succeeded.
     * Layout is family(2) port(2) addr(4), big-endian throughout on 68k. */
    { UWORD z; for (z = 0; z < 16; z++) sabuf[z] = 0; }
    sabuf[0] = 0;  sabuf[1] = 2;                /* sin_family = AF_INET */
    sabuf[2] = (UBYTE)(port >> 8);  sabuf[3] = (UBYTE)port;
    sabuf[4] = (UBYTE)(addr >> 24); sabuf[5] = (UBYTE)(addr >> 16);
    sabuf[6] = (UBYTE)(addr >> 8);  sabuf[7] = (UBYTE)addr;

    /* Connected UDP: one connect() then plain send/recv.  Simpler than
     * sendto/recvfrom, and the kernel filters replies from other hosts. */
    if (connect(fd, (APTR)sabuf, sizeof(sabuf)) < 0) {
        printf("AmiTime: connect failed (errno %ld)\n", (long)Errno());
        CloseSocket(fd);
        return FALSE;
    }

    /* Three attempts: a single lost UDP datagram shouldn't leave the clock
     * unset, and at boot the interface may only just have come up. */
    for (i = 0; i < 3 && !ok; i++) {
        fd_set         rfds;
        struct timeval tv;
        LONG           r;

        memset(pkt, 0, sizeof(pkt));
        pkt[0] = 0x1B;          /* LI=0 (no warning), VN=3, Mode=3 (client) */

        if (send(fd, (APTR)pkt, sizeof(pkt), 0) < 0) {
            printf("AmiTime: send failed (errno %ld)\n", (long)Errno());
            break;
        }

        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        tv.tv_sec  = timeout_secs;
        tv.tv_usec = 0;
        r = WaitSelect(fd + 1, &rfds, NULL, NULL, &tv, NULL);
        if (r <= 0) {
            printf("AmiTime: no reply (attempt %ld of 3)\n", (long)(i + 1));
            continue;
        }

        n = recv(fd, (APTR)pkt, sizeof(pkt), 0);
        if (n < SNTP_PACKET_SIZE) {
            printf("AmiTime: short reply (%ld bytes)\n", (long)n);
            continue;
        }

        /* Mode 4 = server; stratum 0 means "kiss of death", not a real time. */
        if ((pkt[0] & 0x07) != 4 || pkt[1] == 0) {
            printf("AmiTime: server declined the request\n");
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
        if (*ntp_secs == 0) {
            printf("AmiTime: server returned an empty timestamp\n");
            continue;
        }
        ok = TRUE;
    }

    CloseSocket(fd);
    return ok;
}

/* Print an AmigaDOS-epoch time as a readable local date. */
static void print_amiga_time(CONST_STRPTR label, ULONG secs)
{
    struct DateTime dt;
    char  ds[LEN_DATSTRING], ts[LEN_DATSTRING];

    memset(&dt, 0, sizeof(dt));
    dt.dat_Stamp.ds_Days   = (LONG)(secs / 86400);
    dt.dat_Stamp.ds_Minute = (LONG)((secs % 86400) / 60);
    dt.dat_Stamp.ds_Tick   = (LONG)((secs % 60) * TICKS_PER_SECOND);
    dt.dat_Format  = FORMAT_DOS;
    dt.dat_StrDate = (STRPTR)ds;
    dt.dat_StrTime = (STRPTR)ts;
    if (DateToStr(&dt))
        printf("%s %s %s\n", (char *)label, ds, ts);
    else
        printf("%s (%lu)\n", (char *)label, (unsigned long)secs);
}

/* One-shot probe of the local stack's socket conventions.  Different AmigaOS
 * stacks disagree about sockaddr layout and whether a UDP socket wants an
 * explicit protocol, and the failures all look like EAFNOSUPPORT, so measure
 * rather than guess. */
static void diag(STRPTR host)
{
    struct hostent *he;
    ULONG addr = 0;
    LONG  fd;
    struct sockaddr_in sa;

    he = gethostbyname(host);
    if (he && he->h_addr_list && he->h_addr_list[0]) {
        addr = *(ULONG *)he->h_addr_list[0];
        printf("DIAG: resolved %s -> %lu.%lu.%lu.%lu\n", (char *)host,
               (unsigned long)((addr >> 24) & 0xff), (unsigned long)((addr >> 16) & 0xff),
               (unsigned long)((addr >> 8) & 0xff),  (unsigned long)(addr & 0xff));
    } else {
        printf("DIAG: gethostbyname failed (errno %ld)\n", (long)Errno());
        return;
    }

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    printf("DIAG: socket(DGRAM,0)        fd=%ld errno=%ld\n", (long)fd, (long)Errno());
    if (fd >= 0) CloseSocket(fd);

    fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    printf("DIAG: socket(DGRAM,UDP)      fd=%ld errno=%ld\n", (long)fd, (long)Errno());
    if (fd >= 0) {
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;               /* UWORD form: bytes 00 02 */
        sa.sin_port = 123; sa.sin_addr.s_addr = addr;
        printf("DIAG:  connect uword-family  rc=%ld errno=%ld\n",
               (long)connect(fd, (APTR)&sa, sizeof(sa)), (long)Errno());
        CloseSocket(fd);
    }

    fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd >= 0) {
        memset(&sa, 0, sizeof(sa));
        ((UBYTE *)&sa)[0] = 16; ((UBYTE *)&sa)[1] = AF_INET;   /* BSD4.4 form */
        sa.sin_port = 123; sa.sin_addr.s_addr = addr;
        printf("DIAG:  connect sin_len form  rc=%ld errno=%ld\n",
               (long)connect(fd, (APTR)&sa, sizeof(sa)), (long)Errno());
        CloseSocket(fd);
    }

    /* TCP to the same address: if THIS also says 47, the address is the
     * problem; if it says something else, UDP setup is the problem. */
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd >= 0) {
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port = 13; sa.sin_addr.s_addr = addr;
        printf("DIAG: tcp connect            rc=%ld errno=%ld\n",
               (long)connect(fd, (APTR)&sa, sizeof(sa)), (long)Errno());
        CloseSocket(fd);
    }
}

int main(void)
{
    struct RDArgs *rdargs;
    LONG   rda[7];
    UBYTE  hostbuf[128], envbuf[64];
    STRPTR host    = (STRPTR)"pool.ntp.org";
    LONG   tz      = 0;
    LONG   port    = 123;
    LONG   timeout = 5;
    BOOL   showonly = FALSE;
    BOOL   diagmode = FALSE;
    ULONG  ntp_secs = 0, ntp_frac = 0, amiga_secs;
    LONG   i;
    int    rc = RETURN_FAIL;

    /* Defaults from ENV: so User-Startup can stay a bare "C:AmiTime". */
    if (get_env((CONST_STRPTR)"AMITIME_HOST", (STRPTR)hostbuf, sizeof(hostbuf)))
        host = (STRPTR)hostbuf;
    if (get_env((CONST_STRPTR)"AMITIME_TZ", (STRPTR)envbuf, sizeof(envbuf))) {
        LONG v = 0, sign = 1;
        UBYTE *p = envbuf;
        if (*p == '-') { sign = -1; p++; } else if (*p == '+') p++;
        while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
        tz = v * sign;
    }

    rda[0] = rda[1] = rda[2] = rda[3] = rda[4] = rda[5] = rda[6] = 0;
    rdargs = ReadArgs((STRPTR)"HOST/K,TZ/K/N,PORT/K/N,TIMEOUT/K/N,SHOW/S,DIAG/S", rda, NULL);
    if (rdargs) {
        if (rda[0]) {
            STRPTR s = (STRPTR)rda[0];
            for (i = 0; i < (LONG)sizeof(hostbuf) - 1 && s[i]; i++) hostbuf[i] = s[i];
            hostbuf[i] = 0;
            host = (STRPTR)hostbuf;
        }
        if (rda[1]) tz       = *(LONG *)rda[1];
        if (rda[2]) port     = *(LONG *)rda[2];
        if (rda[3]) timeout  = *(LONG *)rda[3];
        if (rda[4]) showonly = TRUE;
        if (rda[5]) diagmode = TRUE;
        FreeArgs(rdargs);
    }
    if (timeout < 1) timeout = 1;

    SocketBase = OpenLibrary((STRPTR)"bsdsocket.library", 4);
    if (!SocketBase) {
        printf("AmiTime: no bsdsocket.library - is the TCP/IP stack running?\n");
        return RETURN_FAIL;
    }

    if (diagmode) {
        diag(host);
        CloseLibrary(SocketBase);
        return RETURN_OK;
    }

    if (sntp_query(host, (UWORD)port, timeout, &ntp_secs, &ntp_frac)) {
        /* NTP epoch -> AmigaDOS epoch, then shift to local time.  AmigaOS keeps
         * the system clock in LOCAL time, so the offset is applied here. */
        if (ntp_secs < NTP_TO_AMIGA_EPOCH) {
            printf("AmiTime: server time predates 1978 - ignoring\n");
        } else {
            amiga_secs = ntp_secs - NTP_TO_AMIGA_EPOCH + (ULONG)(tz * 3600);

            print_amiga_time((CONST_STRPTR)"AmiTime: server time   ", amiga_secs);
            if (showonly) {
                rc = RETURN_OK;
            } else {
                /* Fraction is 1/2^32 s; scale to microseconds without a
                 * 64-bit multiply: (frac >> 12) * 1000000 >> 20. */
                ULONG micro = (ULONG)(((ntp_frac >> 12) / 4295) % 1000000);
                if (set_system_time(amiga_secs, micro)) {
                    printf("AmiTime: system clock set\n");
                    rc = RETURN_OK;
                } else {
                    printf("AmiTime: failed to set the system clock\n");
                }
            }
        }
    }

    CloseLibrary(SocketBase);
    return rc;
}
