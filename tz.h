/*
 * tz.h - POSIX TZ rule parsing and daylight-saving evaluation
 *
 * Copyright (c) 2026 Laine Jones
 * SPDX-License-Identifier: MIT
 *
 * AmigaOS has no daylight-saving support of its own: locale.library gives you
 * loc_GMTOffset and nothing else (loc_Flags is documented "always 0 for now").
 * So if a machine is to keep the right time across a changeover, the rules
 * have to come from somewhere else - this parses the standard POSIX TZ string,
 * the same one Unix uses:
 *
 *     MST7MDT,M3.2.0,M11.1.0        US Mountain
 *     CET-1CEST,M3.5.0,M10.5.0/3    Central Europe
 *     GMT0BST,M3.5.0/1,M10.5.0/2    UK
 *
 * Deliberately kept free of Amiga headers so the logic can be compiled and
 * tested on the build host (see test_tz.c).
 */

#ifndef AMITIME_TZ_H
#define AMITIME_TZ_H

struct TzRule {
    int  valid;        /* 0 = nothing usable was parsed          */
    int  std_east;     /* standard offset, SECONDS EAST of UTC   */
    int  dst_east;     /* daylight offset, seconds east of UTC   */
    int  has_dst;      /* 0 = no daylight part in the string     */
    /* Transition rules, "Mm.w.d/time" form only (the form real TZ strings
     * use).  week 5 means "last".  dow 0 = Sunday. */
    int  start_mon, start_week, start_dow, start_time;
    int  end_mon,   end_week,   end_dow,   end_time;
};

/* Parse a POSIX TZ string.  Returns 1 on success, 0 if it made no sense. */
int tz_parse(const char *s, struct TzRule *out);

/* Seconds east of UTC in force at `utc_secs` (a Unix timestamp), taking
 * daylight saving into account.  With no DST part this is just std_east. */
int tz_offset_at(const struct TzRule *r, long utc_secs);

/* Exposed for testing. */
long tz_days_from_civil(long y, int m, int d);
void tz_civil_from_days(long z, long *y, int *m, int *d);

#endif /* AMITIME_TZ_H */
