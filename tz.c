/*
 * tz.c - POSIX TZ rule parsing and daylight-saving evaluation
 *
 * Copyright (c) 2026 Laine Jones
 * SPDX-License-Identifier: MIT
 */

#include "tz.h"

/* ---- calendar helpers ---------------------------------------------------
 * Howard Hinnant's civil-date algorithms: exact for any year, no tables, no
 * 64-bit maths.  Days are counted from 1970-01-01. */

long tz_days_from_civil(long y, int m, int d)
{
    long era;
    unsigned long yoe, doy, doe;

    y -= (m <= 2);
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = (unsigned long)(y - era * 400);                 /* [0, 399] */
    doy = (153UL * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097L + (long)doe - 719468L;
}

void tz_civil_from_days(long z, long *y, int *m, int *d)
{
    long era, yy;
    unsigned long doe, yoe, doy, mp, dd, mm;

    z += 719468L;
    era = (z >= 0 ? z : z - 146096) / 146097;
    doe = (unsigned long)(z - era * 146097L);             /* [0, 146096] */
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    yy  = (long)yoe + era * 400;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp  = (5 * doy + 2) / 153;
    dd  = doy - (153 * mp + 2) / 5 + 1;
    mm  = mp + (mp < 10 ? 3 : -9);
    *y = yy + (mm <= 2);
    *m = (int)mm;
    *d = (int)dd;
}

/* Day of week for a day-count, 0 = Sunday.  1970-01-01 was a Thursday. */
static int dow_from_days(long days)
{
    int w = (int)((days + 4) % 7);
    return w < 0 ? w + 7 : w;
}

/* ---- parsing ------------------------------------------------------------ */

static const char *skip_name(const char *s)
{
    /* A zone name is either letters, or anything inside <...>. */
    if (*s == '<') {
        while (*s && *s != '>') s++;
        if (*s == '>') s++;
        return s;
    }
    while ((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z')) s++;
    return s;
}

/* POSIX offsets are written as the time to ADD TO LOCAL to reach UTC, so
 * "MST7" means UTC-7.  We return seconds EAST, i.e. the sign flipped. */
static const char *parse_offset(const char *s, int *east, int *found)
{
    int sign = 1, h = 0, m = 0, sec = 0, digits = 0;

    *found = 0;
    if (*s == '+') s++;
    else if (*s == '-') { sign = -1; s++; }

    while (*s >= '0' && *s <= '9') { h = h * 10 + (*s - '0'); s++; digits++; }
    if (!digits) return s;
    if (*s == ':') {
        s++;
        while (*s >= '0' && *s <= '9') { m = m * 10 + (*s - '0'); s++; }
        if (*s == ':') {
            s++;
            while (*s >= '0' && *s <= '9') { sec = sec * 10 + (*s - '0'); s++; }
        }
    }
    *east  = -sign * (h * 3600 + m * 60 + sec);
    *found = 1;
    return s;
}

/* "M<month>.<week>.<dow>[/<time>]" - the only rule form in real TZ strings. */
static const char *parse_rule(const char *s, int *mon, int *week, int *dow, int *time)
{
    int v;

    *mon = *week = *dow = 0;
    *time = 2 * 3600;                     /* POSIX default: 02:00 local */

    if (*s != 'M') return s;              /* J<n> / <n> forms not supported */
    s++;

    v = 0; while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    *mon = v;
    if (*s != '.') return s;
    s++;
    v = 0; while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    *week = v;
    if (*s != '.') return s;
    s++;
    v = 0; while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    *dow = v;

    if (*s == '/') {
        int sign = 1, h = 0, m = 0, sec = 0;
        s++;
        if (*s == '-') { sign = -1; s++; }
        while (*s >= '0' && *s <= '9') { h = h * 10 + (*s - '0'); s++; }
        if (*s == ':') {
            s++;
            while (*s >= '0' && *s <= '9') { m = m * 10 + (*s - '0'); s++; }
            if (*s == ':') {
                s++;
                while (*s >= '0' && *s <= '9') { sec = sec * 10 + (*s - '0'); s++; }
            }
        }
        *time = sign * (h * 3600 + m * 60 + sec);
    }
    return s;
}

int tz_parse(const char *s, struct TzRule *out)
{
    int found = 0;

    out->valid = out->has_dst = 0;
    out->std_east = out->dst_east = 0;
    if (!s || !*s) return 0;

    s = skip_name(s);
    s = parse_offset(s, &out->std_east, &found);
    if (!found) return 0;                 /* a bare name with no offset */
    out->valid = 1;
    out->dst_east = out->std_east;

    if (!*s) return 1;                    /* e.g. "UTC0" - no DST, done */

    /* daylight name, then an optional explicit daylight offset */
    s = skip_name(s);
    {
        int dfound = 0, off = 0;
        const char *after = parse_offset(s, &off, &dfound);
        if (dfound) { out->dst_east = off; s = after; }
        else        { out->dst_east = out->std_east + 3600; }  /* usual +1h */
    }
    out->has_dst = 1;

    /* ",start,end" - without them we cannot know when to switch, so treat
     * the string as standard-time-only rather than guessing. */
    if (*s != ',') { out->has_dst = 0; out->dst_east = out->std_east; return 1; }
    s++;
    s = parse_rule(s, &out->start_mon, &out->start_week, &out->start_dow, &out->start_time);
    if (*s != ',') { out->has_dst = 0; out->dst_east = out->std_east; return 1; }
    s++;
    s = parse_rule(s, &out->end_mon, &out->end_week, &out->end_dow, &out->end_time);

    if (out->start_mon < 1 || out->start_mon > 12 ||
        out->end_mon   < 1 || out->end_mon   > 12) {
        out->has_dst = 0;
        out->dst_east = out->std_east;
    }
    return 1;
}

/* ---- evaluation --------------------------------------------------------- */

/* Unix timestamp of the `week`-th `dow` of `mon` in `year`, at `time`
 * seconds past local midnight.  week 5 means "the last one". */
static long transition(long year, int mon, int week, int dow, int time)
{
    long first = tz_days_from_civil(year, mon, 1);
    int  fdow  = dow_from_days(first);
    long day   = first + ((dow - fdow) + 7) % 7 + (long)(week - 1) * 7;

    if (week >= 5) {
        /* Step back a week at a time until we are still inside the month. */
        long next_mon_first = (mon == 12) ? tz_days_from_civil(year + 1, 1, 1)
                                          : tz_days_from_civil(year, mon + 1, 1);
        while (day >= next_mon_first) day -= 7;
    }
    return day * 86400L + time;
}

int tz_offset_at(const struct TzRule *r, long utc_secs)
{
    long local_std, y, start, end;
    int  m, d;

    if (!r || !r->valid) return 0;
    if (!r->has_dst)     return r->std_east;

    /* POSIX transition times are given in local time; evaluating both ends in
     * local standard time is the usual simplification.  It can only differ in
     * the one ambiguous hour at the autumn changeover, which is immaterial for
     * setting a clock. */
    local_std = utc_secs + r->std_east;
    tz_civil_from_days(local_std / 86400L, &y, &m, &d);

    start = transition(y, r->start_mon, r->start_week, r->start_dow, r->start_time);
    end   = transition(y, r->end_mon,   r->end_week,   r->end_dow,   r->end_time);

    if (start <= end) {
        /* Northern hemisphere: daylight time runs start..end within the year */
        if (local_std >= start && local_std < end) return r->dst_east;
    } else {
        /* Southern hemisphere: daylight time wraps around the new year */
        if (local_std >= start || local_std < end) return r->dst_east;
    }
    return r->std_east;
}
