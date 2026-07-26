# AmiTime

A small SNTP network-time client for AmigaOS. It asks a time server what time
it is and sets the Amiga's clock — useful on any machine whose real-time clock
is missing, has a dead battery, or simply drifts.

It talks only to `bsdsocket.library`, so the same binary runs on **any** Amiga
TCP/IP stack: Roadshow, AmiTCP, Miami, and also
[a314bsd](https://github.com/lainejones/a314bsd) (where the sockets physically
live on a Raspberry Pi). Built for plain 68000 and AmigaOS 2.0+, so one binary
covers an A500 through an 060.

```
1.RAM Disk:> AmiTime TZ -6
AmiTime: 25-Jul-26 17:30:32
AmiTime: system clock set
```

## Why

Amigas with a flat NVRAM battery boot to 1-Jan-78. Beyond the obvious
annoyance, every file you save gets a 1978 timestamp, backups and `Delete
OLDER` logic misbehave, and AmigaDOS starts labelling files "Future". If the
machine is on a network, the time is a few hundred bytes away — this fetches
it.

## Usage

```
AmiTime [HOST <server>] [TZ <hours>] [PORT <n>] [TIMEOUT <secs>]
        [SAVE] [SHOW] [QUIET] [VERSION]
```

| Option | Meaning |
|---|---|
| `HOST`    | Time server. Default `pool.ntp.org`, or `ENV:AMITIME_HOST`. |
| `TZ`      | Hours from UTC, may be negative. Overrides everything below. |
| `PORT`    | Server port (default 123). |
| `TIMEOUT` | Seconds to wait per attempt (default 5; it tries three times). |
| `SAVE`    | Also write the battery-backed clock, like `SetClock save`. |
| `SHOW`    | Print the time but do **not** set any clock. |
| `QUIET`   | Say nothing (for `User-Startup`). |
| `VERSION` | Print the version and exit. |

Examples:

```
AmiTime                              ; pool.ntp.org, timezone from Locale
AmiTime TZ -6                        ; explicit offset (US Mountain)
AmiTime HOST 192.168.1.10 TZ 1 SAVE  ; LAN server, set RTC too
AmiTime SHOW                         ; just tell me the time
```

## Timezones and daylight saving

AmigaOS keeps the system clock in **local** time, so AmiTime has to know your
offset from UTC. In order of preference it uses:

1. the `TZ` argument — a fixed offset,
2. `ENV:AMITIME_TZ` — a fixed offset,
3. **a POSIX TZ rule** in `ENV:AMITIME_TZRULE` or `ENV:TZ` — handles daylight saving,
4. your Locale preferences (`loc_GMTOffset`) — a fixed offset,
5. otherwise UTC.

It always prints which one it used.

### Daylight saving

AmigaOS itself has no concept of daylight saving — `locale.library` gives you a
GMT offset and nothing more (`loc_Flags` is documented "always 0 for now"). So
for the clock to stay right across a changeover, the rules have to come from a
**POSIX TZ string**, the same format Unix uses:

```
SetEnv SAVE AMITIME_TZRULE "MST7MDT,M3.2.0,M11.1.0"    ; US Mountain
SetEnv SAVE AMITIME_TZRULE "CET-1CEST,M3.5.0,M10.5.0/3" ; Central Europe
SetEnv SAVE AMITIME_TZRULE "GMT0BST,M3.5.0/1,M10.5.0/2" ; UK
SetEnv SAVE AMITIME_TZRULE "AEST-10AEDT,M10.1.0,M4.1.0/3" ; Sydney
SetEnv SAVE AMITIME_TZRULE "JST-9"                      ; Japan, no DST
```

AmiTime then works out whether daylight saving is in force **for the date it
just fetched** and says so:

```
AmiTime: using TZ rule 'MST7MDT,M3.2.0,M11.1.0' (-360 minutes from UTC, daylight saving)
AmiTime: 25-Jul-26 19:05:45
```

Note the POSIX sign convention: the offset is how far you are *behind* UTC, so
US Mountain is `MST7`, and Central Europe is `CET-1`. Supported rule form is
`Mmonth.week.day[/time]` (week 5 meaning "last"), which is what real TZ strings
use. Southern-hemisphere rules that wrap the new year work correctly.

Passing `TZ` explicitly overrides the rule, so a fixed offset always wins if
that is what you want.

## Installing

Run the supplied installer from a Shell in this drawer:

```
Execute Install
```

It copies `AmiTime` to `C:`, optionally asks for a server and timezone and
stores them in `ENVARC:`, and can add a line to `S:User-Startup` so the clock
is set at every boot.

To do it by hand instead:

```
Copy AmiTime C:
SetEnv SAVE AMITIME_HOST pool.ntp.org
SetEnv SAVE AMITIME_TZ -6
```

and add to `S:User-Startup`, *after* your TCP/IP stack starts:

```
C:AmiTime QUIET
```

On a machine with a working battery clock, `C:AmiTime SAVE QUIET` will also
keep the RTC corrected. If your stack takes a while to come up (a314bsd waits
on a Raspberry Pi, for example), background it and allow more time:

```
Run >NIL: C:AmiTime QUIET TIMEOUT 20
```

## Requirements

- AmigaOS 2.0 or newer
- A TCP/IP stack providing `bsdsocket.library`
- A reachable SNTP/NTP server — the internet, or one on your LAN

## Tested on

| Machine | Chipset / CPU | Stack + hardware |
|---|---|---|
| A4000 | AGA, 68060 | Roadshow over MNT ZZ9000 ethernet |
| A2000 | **ECS**, 68030 (no FPU) | Roadshow over Individual Computers X-Surf |
| A1200 | AGA, 68030 | [a314bsd](https://github.com/lainejones/a314bsd) — sockets on a Raspberry Pi over A314 |

All on AmigaOS 3.2.3, against both a LAN time server and `pool.ntp.org`. The
A4000 and A2000 also had their battery clocks written with `SAVE`.

## Building

Built with the [bebbo amiga-gcc](https://github.com/bebbo/amiga-gcc)
cross-compiler. The repository is self-contained: it uses only the NDK's own
headers.

```
make            build AmiTime
make test       run the timezone tests on the build host
make dist       build the release archives
```

The daylight-saving logic lives in `tz.c`, deliberately free of Amiga headers
so it can be compiled and tested natively — `make test` checks it against both
changeover boundaries, "last Sunday" rules, southern-hemisphere zones that wrap
the new year, and zones without DST.

## Notes for the curious

Two things in the source look odd and are deliberate — both are recorded in
comments so nobody "fixes" them:

- **The socket address is built as `volatile` bytes.** At `-O2` this compiler
  merges the adjacent 16-bit stores of `sin_family`/`sin_port` into one 32-bit
  store and drops the family, producing a sockaddr of `0000 007b` instead of
  `0002 007b`. The stack then rejects the socket with `EAFNOSUPPORT`.
- **UDP sockets are created with `IPPROTO_UDP`, not `0`.** Roadshow will
  create a protocol-0 datagram socket quite happily and then refuse to
  `connect()` it.

The NTP epoch (1900) to AmigaDOS epoch (1978) difference is 2,461,449,600
seconds.

## Licence

MIT — see [LICENSE](LICENSE).
