/*
 * test_tz.c - host-side tests for the POSIX TZ logic
 *
 * Copyright (c) 2026 Laine Jones
 * SPDX-License-Identifier: MIT
 *
 * Built and run on the BUILD machine (make test), not the Amiga: the date
 * arithmetic is ordinary C, and checking it here is far quicker than pushing
 * a binary to real hardware for every edge case.
 */

#include <stdio.h>
#include <string.h>
#include "tz.h"

static int failures = 0;

static long utc(long y, int mo, int d, int h, int mi)
{
    return tz_days_from_civil(y, mo, d) * 86400L + h * 3600L + mi * 60L;
}

static void check(const char *what, long got, long want)
{
    if (got == want) {
        printf("  ok    %-52s %ld\n", what, got);
    } else {
        printf("  FAIL  %-52s got %ld want %ld\n", what, got, want);
        failures++;
    }
}

static void check_off(const char *tz, long t, int want, const char *what)
{
    struct TzRule r;
    if (!tz_parse(tz, &r)) { printf("  FAIL  cannot parse %s\n", tz); failures++; return; }
    check(what, tz_offset_at(&r, t), want);
}

int main(void)
{
    struct TzRule r;

    printf("calendar round-trip\n");
    {
        long y; int m, d;
        tz_civil_from_days(tz_days_from_civil(2026, 7, 25), &y, &m, &d);
        check("2026-07-25 round trip (y)", y, 2026);
        check("2026-07-25 round trip (m)", m, 7);
        check("2026-07-25 round trip (d)", d, 25);
        check("days at unix epoch",           tz_days_from_civil(1970, 1, 1), 0);
        check("days at 2000-03-01 (leap yr)", tz_days_from_civil(2000, 3, 1), 11017);
    }

    printf("\nparsing\n");
    check("MST7MDT parses",            tz_parse("MST7MDT,M3.2.0,M11.1.0", &r), 1);
    check("  std offset is UTC-7",     r.std_east, -7 * 3600);
    check("  dst offset is UTC-6",     r.dst_east, -6 * 3600);
    check("  has dst",                 r.has_dst, 1);
    check("UTC0 parses",               tz_parse("UTC0", &r), 1);
    check("  no dst",                  r.has_dst, 0);
    check("empty string rejected",     tz_parse("", &r), 0);
    check("name-only rejected",        tz_parse("MST", &r), 0);
    check("CET-1CEST std is UTC+1",
          (tz_parse("CET-1CEST,M3.5.0,M10.5.0/3", &r), r.std_east), 3600);

    printf("\nUS Mountain (MST7MDT,M3.2.0,M11.1.0)\n");
    /* 2026: DST starts Sun 8 Mar, ends Sun 1 Nov */
    check_off("MST7MDT,M3.2.0,M11.1.0", utc(2026, 1, 15, 12, 0), -7 * 3600, "mid-January is MST");
    check_off("MST7MDT,M3.2.0,M11.1.0", utc(2026, 7, 25, 23, 0), -6 * 3600, "late July is MDT");
    check_off("MST7MDT,M3.2.0,M11.1.0", utc(2026, 3,  8,  8, 0), -7 * 3600, "just before spring change");
    check_off("MST7MDT,M3.2.0,M11.1.0", utc(2026, 3,  8, 10, 0), -6 * 3600, "just after spring change");
    check_off("MST7MDT,M3.2.0,M11.1.0", utc(2026, 11, 1,  7, 0), -6 * 3600, "just before autumn change");
    check_off("MST7MDT,M3.2.0,M11.1.0", utc(2026, 11, 1, 10, 0), -7 * 3600, "just after autumn change");
    check_off("MST7MDT,M3.2.0,M11.1.0", utc(2026, 12, 25, 12, 0), -7 * 3600, "Christmas is MST");

    printf("\nCentral Europe (CET-1CEST,M3.5.0,M10.5.0/3) - 'last Sunday' rules\n");
    check_off("CET-1CEST,M3.5.0,M10.5.0/3", utc(2026, 1, 15, 12, 0), 3600,     "January is CET");
    check_off("CET-1CEST,M3.5.0,M10.5.0/3", utc(2026, 7, 15, 12, 0), 2 * 3600, "July is CEST");
    check_off("CET-1CEST,M3.5.0,M10.5.0/3", utc(2026, 3, 29, 12, 0), 2 * 3600, "after last-Sun-March change");
    check_off("CET-1CEST,M3.5.0,M10.5.0/3", utc(2026, 10, 25, 12, 0), 3600,    "after last-Sun-October change");

    printf("\nSouthern hemisphere (NZST-12NZDT,M9.5.0,M4.1.0) - wraps the new year\n");
    check_off("NZST-12NZDT,M9.5.0,M4.1.0", utc(2026, 1, 15, 0, 0), 13 * 3600, "January is NZDT");
    check_off("NZST-12NZDT,M9.5.0,M4.1.0", utc(2026, 6, 15, 0, 0), 12 * 3600, "June is NZST");

    printf("\nno-DST zones\n");
    check_off("UTC0",   utc(2026, 7, 25, 12, 0), 0,          "UTC stays UTC in summer");
    check_off("JST-9",  utc(2026, 7, 25, 12, 0), 9 * 3600,   "Japan has no DST");
    check_off("IST-5:30", utc(2026, 7, 25, 12, 0), 5 * 3600 + 1800, "India is +5:30");

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "all tests passed",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
