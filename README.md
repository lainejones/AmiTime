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
| `TZ`      | Hours from UTC, may be negative. Default: your Locale setting, or `ENV:AMITIME_TZ`. |
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

## Timezones

AmigaOS keeps the system clock in **local** time, so AmiTime has to know your
offset from UTC. In order of preference it uses:

1. the `TZ` argument,
2. `ENV:AMITIME_TZ`,
3. your Locale preferences (`loc_GMTOffset`).

It prints which one it used. Be aware that a lot of Amigas have never had
Locale's GMT offset set — if the reported offset looks wrong, either fix it in
`Prefs/Locale` or pass `TZ` explicitly.

There is no automatic daylight-saving handling: if your region changes clocks,
update `TZ` (or your Locale) when it does.

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

## Building

Built with the [bebbo amiga-gcc](https://github.com/bebbo/amiga-gcc)
cross-compiler. The repository is self-contained: it uses only the NDK's own
headers.

```
make
```

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
