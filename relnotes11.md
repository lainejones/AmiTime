Adds daylight-saving support.

AmigaOS has no concept of daylight saving of its own — `locale.library` gives
you a GMT offset and nothing more (`loc_Flags` is documented "always 0 for
now"). So AmiTime now understands a **POSIX TZ rule string**, the same format
Unix uses, and works out whether daylight saving is in force for the date it
just fetched:

```
SetEnv SAVE AMITIME_TZRULE "MST7MDT,M3.2.0,M11.1.0"
```

```
AmiTime: using TZ rule 'MST7MDT,M3.2.0,M11.1.0' (-360 minutes from UTC, daylight saving)
AmiTime: 25-Jul-26 19:05:45
```

Set it once and the clock stays right across the spring and autumn
changeovers, with no further attention.

### What's new

- POSIX TZ rules from `ENV:AMITIME_TZRULE` or `ENV:TZ`, including
  `Mmonth.week.day[/time]` rules ("last Sunday" and all), zones with no DST,
  and southern-hemisphere rules that wrap the new year.
- AmiTime always reports which source its offset came from — argument, ENV,
  TZ rule, Locale, or UTC — so a wrong clock is never silent.
- `make test` runs the timezone logic on the build host, checking both
  changeover boundaries and several regions. The logic lives in `tz.c`, kept
  free of Amiga headers precisely so it can be tested that way.

A fixed `TZ` argument still overrides everything, if that is what you prefer.

### Installing

Download `AmiTime-1.1.lha` (Amiga) or `AmiTime-1.1.zip`, unpack, and run
`Execute Install` from that drawer. See the README for all options.

Unchanged from 1.0: one 68000 binary for any Amiga with a TCP/IP stack,
verified on an A4000 (AGA/060, Roadshow over MNT ZZ9000), an A2000 (ECS/030,
Roadshow over X-Surf) and an A1200 (a314bsd, sockets on a Raspberry Pi).
