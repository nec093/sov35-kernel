#ifndef _LINUX_TIMEKEEPING32_H
#define _LINUX_TIMEKEEPING32_H
/*
 * These interfaces are all based on the old timespec type
 * and should get replaced with the timespec64 based versions
 * over time so we can remove the file here.
 */

static inline unsigned long get_seconds(void)
{
	return ktime_get_real_seconds();
}

/*
 * CAF addition: do_gettimeofday() was removed in 5.0; the msm camera/video
 * drivers still fill struct timeval fields of their uapi structs with it.
 */
static inline void do_gettimeofday(struct timeval *tv)
{
	struct timespec64 ts64;

	ktime_get_real_ts64(&ts64);
	tv->tv_sec = ts64.tv_sec;
	tv->tv_usec = ts64.tv_nsec / NSEC_PER_USEC;
}

static inline void getnstimeofday(struct timespec *ts)
{
	struct timespec64 ts64;

	ktime_get_real_ts64(&ts64);
	*ts = timespec64_to_timespec(ts64);
}

static inline void ktime_get_ts(struct timespec *ts)
{
	struct timespec64 ts64;

	ktime_get_ts64(&ts64);
	*ts = timespec64_to_timespec(ts64);
}

static inline void getrawmonotonic(struct timespec *ts)
{
	struct timespec64 ts64;

	ktime_get_raw_ts64(&ts64);
	*ts = timespec64_to_timespec(ts64);
}

static inline void getboottime(struct timespec *ts)
{
	struct timespec64 ts64;

	getboottime64(&ts64);
	*ts = timespec64_to_timespec(ts64);
}

/*
 * CAF addition (not in mainline): get_monotonic_boottime() (monotonic
 * time since boot, i.e. including suspend) was removed upstream in
 * favor of ktime_get_boottime_ts64(); reintroduced here as a struct
 * timespec-based compat wrapper, same pattern as the other shims in
 * this file.
 */
static inline void get_monotonic_boottime(struct timespec *ts)
{
	struct timespec64 ts64;

	ktime_get_boottime_ts64(&ts64);
	*ts = timespec64_to_timespec(ts64);
}

#endif
