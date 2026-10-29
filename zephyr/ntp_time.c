#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/rtc.h>
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/sntp.h>
#include <zephyr/net/socket.h>
#include <zephyr/posix/time.h>

/* ntp_set_user_server() is defined below and called from ntp_config.cpp. */
#include "ntp_config.h"

LOG_MODULE_REGISTER(ntp, LOG_LEVEL_INF);

/* From zephyr/Kconfig. sizeof("") == 1, so the array size tells at compile
 * time whether a default server was configured. This is the built-in address:
 * it is used when the user has not set one, or when theirs is unreachable. */
static const char ntp_server[] = CONFIG_OPENBMC_PHOSPHOR_TIME_MANAGER_NTP_SERVER;

/* Defined in timedated_main.cpp. While disabled this thread does not sync, so
 * a manually set time is not overwritten. */
extern bool ntp_is_enabled(void);

/* Defined in timedated_main.cpp. Given when the switch state is first known. */
extern struct k_sem ntp_mode_known;

/* Addresses set from the web UI / Redfish, in the order they were given,
 * written by ntp_config.cpp through ntp_set_user_servers(). The count is 0
 * when nothing is set, and ntp_server above is then the only server.
 *
 * Kept in this C file rather than ntp_config.cpp so K_MUTEX_DEFINE stays in C,
 * where its designated initialiser is less compiler-sensitive. */
static char userServer[NTP_MAX_SERVERS][NTP_USER_SERVER_MAX];
static int userServerCount;
static K_MUTEX_DEFINE(userServerLock);

/* The list is saved under /overlay, the writable filesystem on this image
 * (there is no /var), one address per line. It is written on every change,
 * which only happens when a user sets one, so the blocking littlefs write is
 * not on a hot path. */
#define NTP_USER_SERVER_DIR  "/overlay/var/lib/phosphor-time-manager"
#define NTP_USER_SERVER_PATH NTP_USER_SERVER_DIR "/ntp_server"

/* fs_write() is allowed to return fewer bytes than asked for. littlefs does not
 * do that today, but a partial write would glue two addresses together in the
 * one-per-line file, so insist on the full length every time. */
static bool write_all(struct fs_file_t *f, const char *data, size_t len)
{
	return fs_write(f, data, len) == (ssize_t)len;
}

static void save_user_servers(const char list[][NTP_USER_SERVER_MAX],
			      int count)
{
	struct fs_file_t f;
	bool ok = true;
	int closed;
	int i;

	fs_file_t_init(&f);

	/* -EEXIST when it is already there, which is the normal case. */
	(void)fs_mkdir(NTP_USER_SERVER_DIR);

	if (fs_open(&f, NTP_USER_SERVER_PATH,
		    FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC) != 0) {
		LOG_ERR("cannot save the server list; keeping the previous "
			"one");
		return;
	}

	for (i = 0; i < count && ok; i++) {
		ok = write_all(&f, list[i], strlen(list[i])) &&
		     write_all(&f, "\n", 1);
	}

	closed = fs_close(&f);

	/* littlefs commits on close, so a failure there matters as much. */
	if (!ok || closed != 0) {
		/* The file was truncated when it was opened and is now only
		 * partly written. Keeping it would make the next boot restore a
		 * truncated list, so drop it: the Kconfig default is a better
		 * answer than a list nobody typed. */
		(void)fs_unlink(NTP_USER_SERVER_PATH);
		LOG_ERR("cannot save the server list (%d); removed the saved "
			"one so the built-in server is used after a reboot",
			closed);
		LOG_INF("set the servers again in the web UI (Settings -> "
			"Date & Time) to retry saving them");
	}
}

/* Set only once the load reached a final outcome (list parsed, file empty, or
 * nothing saved yet), under userServerLock. A transient I/O error leaves it
 * clear so a later round retries. */
static bool userServersLoaded;

/* Restore the list saved by a previous run. The sync thread calls it before its
 * first round; ntp_get_user_servers() calls it too, so the property does not
 * depend on who runs first. */
static void load_user_servers(void)
{
	struct fs_file_t f;
	char buf[NTP_MAX_SERVERS * (NTP_USER_SERVER_MAX + 1)];
	ssize_t n;
	char *line;
	int count = 0;
	int rc;

	/* Held across the read: a caller arriving while the parse runs would
	 * otherwise see the flag and get a half-filled list. This runs once at
	 * boot, so holding it across flash I/O is fine. */
	k_mutex_lock(&userServerLock, K_FOREVER);

	if (userServersLoaded) {
		k_mutex_unlock(&userServerLock);
		return;
	}

	fs_file_t_init(&f);

	rc = fs_open(&f, NTP_USER_SERVER_PATH, FS_O_READ);
	if (rc != 0) {
		if (rc == -ENOENT) {
			/* Nothing has been saved yet: a final state. */
			userServersLoaded = true;
		} else {
			/* Transient: leave the flag clear so a later round
			 * retries instead of losing the saved list for this
			 * boot. */
			LOG_ERR("cannot read the saved server list (%d)", rc);
		}
		k_mutex_unlock(&userServerLock);
		return;
	}
	n = fs_read(&f, buf, sizeof(buf) - 1);
	fs_close(&f);
	if (n < 0) {
		LOG_ERR("cannot read the saved server list (%d)", (int)n);
		k_mutex_unlock(&userServerLock);
		return;
	}
	if (n == 0) {
		userServersLoaded = true;
		k_mutex_unlock(&userServerLock);
		return; /* empty file */
	}
	buf[n] = '\0';

	line = buf;
	while (*line != '\0' && count < NTP_MAX_SERVERS) {
		char *eol = strchr(line, '\n');
		size_t len;

		if (eol != NULL) {
			*eol = '\0';
		}

		/* Tolerate CRLF in case the file was edited elsewhere. */
		len = strlen(line);
		if (len > 0 && line[len - 1] == '\r') {
			line[len - 1] = '\0';
		}

		if (line[0] != '\0') {
			snprintf(userServer[count], NTP_USER_SERVER_MAX, "%s",
				 line);
			LOG_INF("restored server %s", userServer[count]);
			count++;
		}

		if (eol == NULL) {
			break;
		}
		line = eol + 1;
	}

	userServerCount = count;
	userServersLoaded = true;
	k_mutex_unlock(&userServerLock);
}

/* The sync thread sleeps NTP_RESYNC_SEC between rounds. Changing the server
 * address or toggling NTP should take effect immediately, so the sleep is a
 * k_sem_take() that can be interrupted, and ntp_wake() gives it.
 * Count is capped at 1, so repeated triggers collapse into one extra round. */
static K_SEM_DEFINE(ntp_wake_sem, 0, 1);

/* Wake the sync thread so it starts over. Defined here, called from C++
 * (timedated). Called when the server address changes and when the NTP
 * switch is toggled. Safe to call from the io thread. */
void ntp_wake(void)
{
	k_sem_give(&ntp_wake_sem);
}

void ntp_set_user_servers(const char *const *servers, int count)
{
	char snapshot[NTP_MAX_SERVERS][NTP_USER_SERVER_MAX];
	int n = 0;
	int i;

	k_mutex_lock(&userServerLock, K_FOREVER);
	for (i = 0; i < count && n < NTP_MAX_SERVERS; i++) {
		if (servers[i] == NULL || servers[i][0] == '\0') {
			continue;
		}
		snprintf(userServer[n], NTP_USER_SERVER_MAX, "%s", servers[i]);
		n++;
	}
	userServerCount = n;
	for (i = 0; i < n; i++) {
		snprintf(snapshot[i], NTP_USER_SERVER_MAX, "%s", userServer[i]);
	}
	k_mutex_unlock(&userServerLock);

	/* Write outside the lock: this blocks on flash, and the sync thread
	 * only needs the lock for the duration of a copy. Only the timedated io
	 * thread calls this today, so two saves cannot interleave; if a second
	 * caller ever appears, give save_user_servers() its own mutex. */
	save_user_servers(snapshot, n);

	if (n == 0) {
		LOG_INF("user servers cleared");
	} else {
		for (i = 0; i < n; i++) {
			LOG_INF("user server %d: %s", i, snapshot[i]);
		}
	}

	ntp_wake();
}

int ntp_get_user_servers(char out[][NTP_USER_SERVER_MAX], int max)
{
	int n;
	int i;

	/* The sync thread loads at start-up, but the property initialiser runs
	 * on the io thread; load here as well so it never reports an empty list
	 * just because it got there first. */
	load_user_servers();

	k_mutex_lock(&userServerLock, K_FOREVER);
	n = (userServerCount < max) ? userServerCount : max;
	for (i = 0; i < n; i++) {
		snprintf(out[i], NTP_USER_SERVER_MAX, "%s", userServer[i]);
	}
	k_mutex_unlock(&userServerLock);

	return n;
}

#define NTP_TIMEOUT_MS   3000 /* per-query timeout */
#define NTP_RESYNC_SEC   3600 /* interval after a successful sync */
#define NTP_RETRY_SEC    10   /* interval after a failure */
#define NTP_NET_WAIT_SEC 5    /* network poll interval */

/* An unreachable server is retried forever, but the rate changes: the first few
 * attempts run every NTP_RETRY_SEC and are reported, because a console line
 * every 10 s floods everything else. Once those are spent the thread drops to
 * NTP_SLOW_RETRY_SEC and says so once, pointing at the web UI. It keeps
 * retrying quietly, so a network that comes back is picked up within one slow
 * period instead of needing the user to do anything. */
#define NTP_LOUD_FAILURES   3
#define NTP_SLOW_RETRY_SEC  3600 /* 1 h */

/* Accepted range for the time received over NTP. The bounds are the limits of
 * the IPMI SEL timestamp field, which is a uint32 count of seconds since 1970;
 * anything outside it is truncated in SEL into a plausible-looking but wrong
 * date. The same bounds are used in app/src/rtc_time.c and timedated_main.cpp.
 *
 * A bad value must not be written: the hardware RTC is the only copy that
 * survives power loss, and overwriting it is irreversible. */
#define NTP_MIN_VALID_EPOCH 0LL          /* 1970-01-01, Unix epoch */
#define NTP_MAX_VALID_EPOCH 4294967295LL /* 2106-02-07, uint32 limit */

static bool net_ready(void)
{
	const struct net_if *iface = net_if_get_default();
	struct in_addr *addr;

	if (iface == NULL || !net_if_is_up(iface)) {
		return false;
	}

	/* Static IPv4 is configured by net_stack_cfg (CONFIG_NET_CONFIG_MY_IPV4_ADDR) */
	addr = net_if_ipv4_get_global_addr((struct net_if *)iface, NET_ADDR_PREFERRED);
	return (addr != NULL && !net_ipv4_is_addr_unspecified(addr));
}

/* Turn the configured server string into an IPv4 address.
 *
 * Tried as a literal first; if that fails it is looked up as a host name,
 * which needs CONFIG_DNS_RESOLVER and a DNS server address (CONFIG_DNS_SERVER1)
 * since there is no DHCP here.
 *
 * Not cached: resolution happens once per sync round, which is one hour apart
 * on success, and follows the 10 s retry on failure so a DNS recovery is
 * picked up automatically. */
static int resolve_server(const char *host, struct in_addr *out)
{
	struct zsock_addrinfo hints = {
		.ai_family = AF_INET,
		.ai_socktype = SOCK_DGRAM,
	};
	struct zsock_addrinfo *res = NULL;
	char buf[INET_ADDRSTRLEN];
	int rc;

	/* Literal IPv4 -- fastest path, no DNS involved. */
	if (net_addr_pton(AF_INET, host, out) == 0) {
		return 0;
	}

	/* service is NULL: the port comes from the caller's sockaddr_in. */
	rc = zsock_getaddrinfo(host, NULL, &hints, &res);
	if (rc != 0 || res == NULL) {
		LOG_ERR("cannot resolve '%s' (getaddrinfo %d)", host, rc);
		return -EINVAL;
	}

	*out = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
	zsock_freeaddrinfo(res);

	LOG_DBG("resolved %s -> %s", host,
	       net_addr_ntop(AF_INET, out, buf, sizeof(buf)));

	return 0;
}

/* Query a server once. Returns 0 and fills *out on success.
 * Separate so a failed attempt can be repeated with another address. */
static int query_once(const char *host, struct sntp_time *out)
{
	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_port = htons(CONFIG_OPENBMC_PHOSPHOR_TIME_MANAGER_NTP_PORT),
	};
	struct sntp_ctx ctx;
	int rc;

	rc = resolve_server(host, &addr.sin_addr);
	if (rc != 0) {
		return rc;
	}

	if (sntp_init(&ctx, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		LOG_ERR("sntp_init failed");
		return -EIO;
	}

	rc = sntp_query(&ctx, NTP_TIMEOUT_MS, out);
	sntp_close(&ctx);

	return rc;
}

/* Report how far the clock moved, so a large correction stands out from the
 * millisecond drift of an ordinary hourly resync. deltaUs is new - old: a
 * positive value means the clock was behind and we moved it forward. */
static void print_clock_delta(int64_t deltaUs)
{
	int64_t absUs = deltaUs < 0 ? -deltaUs : deltaUs;
	const char *direction = deltaUs < 0 ? "ahead" : "behind";
	char value[48];

	if (absUs < 1000) {
		snprintf(value, sizeof(value), "%lld us", (long long)absUs);
	} else if (absUs < 1000000) {
		snprintf(value, sizeof(value), "%lld.%03lld ms",
			 (long long)(absUs / 1000), (long long)(absUs % 1000));
	} else if (absUs < 86400LL * 1000000) {
		snprintf(value, sizeof(value), "%lld.%03lld s",
			 (long long)(absUs / 1000000),
			 (long long)((absUs % 1000000) / 1000));
	} else {
		snprintf(value, sizeof(value), "%lld.%03lld s (~%lld days)",
			 (long long)(absUs / 1000000),
			 (long long)((absUs % 1000000) / 1000),
			 (long long)(absUs / (86400LL * 1000000)));
	}

	LOG_INF("clock was %s %s", value, direction);
}

/* Write the current system time to the hardware RTC.
 *
 * Duplicates timedated_main.cpp; the two copies should be merged into a single
 * helper in app/src/rtc_time.c. */
static void write_rtc_from_system(void)
{
	const struct device *rtc = DEVICE_DT_GET(DT_NODELABEL(rtc));
	struct rtc_time rtc_tm = {0};
	struct timespec ts;
	struct tm tm;

	if (!device_is_ready(rtc)) {
		LOG_WRN("rtc not ready, skip");
		return;
	}
	if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
		LOG_ERR("clock_gettime failed");
		return;
	}
	if (gmtime_r(&ts.tv_sec, &tm) == NULL) {
		LOG_ERR("gmtime_r failed");
		return;
	}

	rtc_tm.tm_sec = tm.tm_sec;
	rtc_tm.tm_min = tm.tm_min;
	rtc_tm.tm_hour = tm.tm_hour;
	rtc_tm.tm_mday = tm.tm_mday;
	rtc_tm.tm_mon = tm.tm_mon;
	rtc_tm.tm_year = tm.tm_year;
	rtc_tm.tm_wday = tm.tm_wday;

	if (rtc_set_time(rtc, &rtc_tm) != 0) {
		LOG_ERR("rtc_set_time failed");
		return;
	}

	LOG_DBG("RTC <- %d-%02u-%02u %02u:%02u:%02u UTC",
	       tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
	       tm.tm_hour, tm.tm_min, tm.tm_sec);
}

static void ntp_sync_thread(void *p1, void *p2, void *p3)
{
	struct sntp_time ts;
	struct timespec tspec;
	int failures = 0; /* consecutive failed rounds; drives how loud the log is */

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	/* Bring back the list a previous run saved, before the first sync. */
	load_user_servers();

	/* Without a built-in default the thread is still useful when a saved
	 * list exists, so give up only if both are missing. */
	if (sizeof(ntp_server) == 1) {
		char saved[NTP_MAX_SERVERS][NTP_USER_SERVER_MAX];

		if (ntp_get_user_servers(saved, NTP_MAX_SERVERS) == 0) {
			LOG_WRN("NTP_SERVER not configured, disabled");
			return;
		}
		LOG_INF("no default server, using the saved list");
	} else {
		LOG_INF("default server %s:%d", ntp_server,
			CONFIG_OPENBMC_PHOSPHOR_TIME_MANAGER_NTP_PORT);
	}

	/* Wait until the switch state is known. The default is "on", and the
	 * persisted mode only reaches timedated from manager.cpp about 15 s
	 * into boot, so without this wait a manual-mode boot would sync once
	 * before that. The timeout keeps NTP working if the mode never
	 * arrives (settings-manager failed to start). */
	(void)k_sem_take(&ntp_mode_known, K_SECONDS(30));

	for (;;) {
		uint32_t wait_sec = NTP_RESYNC_SEC;
		char userbuf[NTP_MAX_SERVERS][NTP_USER_SERVER_MAX];
		const char *host;
		int n_user;
		int i;
		int rc;

		/* Skip while NTP is switched off (manual mode). Wait on the
		 * wake semaphore rather than sleeping, so switching NTP back
		 * on takes effect at once, not after NTP_NET_WAIT_SEC. */
		if (!ntp_is_enabled()) {
			(void)k_sem_take(&ntp_wake_sem,
					 K_SECONDS(NTP_NET_WAIT_SEC));
			continue;
		}

		/* Wait for the network (static IP configured by net_stack_cfg). */
		while (!net_ready()) {
			k_sleep(K_SECONDS(NTP_NET_WAIT_SEC));
		}

		/* Drop any wake signal raised before this point: the round below
		 * reads the state that signal was about, so acting on it again
		 * would only run a redundant round. A signal raised after this
		 * point is kept and makes the k_sem_take() at the end return at
		 * once.
		 *
		 * The reset has to come before the state is read, not after.
		 * Between the two, a new server address would otherwise be
		 * neither read here nor signalled to the next round, so it would
		 * not take effect for a full resync period. */
		k_sem_reset(&ntp_wake_sem);

		/* The user's addresses win when set, tried in the order they
		 * were given; the built-in one is only used when none of them
		 * answers. */
		n_user = ntp_get_user_servers(userbuf, NTP_MAX_SERVERS);
		host = ntp_server;
		rc = -EINVAL;

		for (i = 0; i < n_user; i++) {
			/* Stop the round as soon as the switch goes off, so
			 * the remaining servers are not queried for nothing. */
			if (!ntp_is_enabled()) {
				break;
			}
			rc = query_once(userbuf[i], &ts);
			if (rc == 0) {
				host = userbuf[i];
				break;
			}
			LOG_WRN("%s unreachable (%d)", userbuf[i], rc);
		}

		/* Only worth falling back when a built-in server exists and
		 * the switch is still on. */
		if (rc != 0 && ntp_is_enabled() && sizeof(ntp_server) > 1) {
			if (n_user > 0) {
				LOG_INF("no user server answered, "
					"fallback to %s", ntp_server);
			}
			host = ntp_server;
			rc = query_once(host, &ts);
		}

		if (!ntp_is_enabled()) {
			/* Switched off while the round was running. Dropped
			 * silently: no failure counted, no clock write. This
			 * also covers a switch-off during a query. */
			wait_sec = NTP_RETRY_SEC;
		} else if (rc != 0) {
			failures++;
			if (failures <= NTP_LOUD_FAILURES) {
				LOG_WRN("query failed (%d/%d), retry in %ds",
					failures, NTP_LOUD_FAILURES,
					NTP_RETRY_SEC);
				wait_sec = NTP_RETRY_SEC;
			} else {
				if (failures == NTP_LOUD_FAILURES + 1) {
					LOG_ERR("no NTP server is reachable; "
						"switching to slow mode and "
						"retrying every %d min",
						NTP_SLOW_RETRY_SEC / 60);
					LOG_INF("set a server in the web UI "
						"(Settings -> Date & Time) to "
						"sync right away");
				}
				wait_sec = NTP_SLOW_RETRY_SEC;
			}
		} else if (ts.seconds < NTP_MIN_VALID_EPOCH ||
			   ts.seconds > NTP_MAX_VALID_EPOCH) {
			/* Implausible value: no system time, and no RTC write. */
			LOG_ERR("implausible %llu from %s, ignored",
				(unsigned long long)ts.seconds, host);
			wait_sec = NTP_RETRY_SEC;
		} else if (!ntp_is_enabled()) {
			/* Off since the check above: a manually set time must
			 * never be overwritten. */
			LOG_INF("disabled during query, skip write");
			wait_sec = NTP_RETRY_SEC;
		} else {
			struct timespec before = {0};
			bool haveBefore =
				(clock_gettime(CLOCK_REALTIME, &before) == 0);

			tspec.tv_sec = (time_t)ts.seconds;
			tspec.tv_nsec =
				((uint64_t)ts.fraction * NSEC_PER_SEC) >> 32;

			if (clock_settime(CLOCK_REALTIME, &tspec) != 0) {
				LOG_ERR("clock_settime failed");
				wait_sec = NTP_RETRY_SEC;
			} else {
				LOG_INF("synced to %llu via %s",
					(unsigned long long)ts.seconds, host);
				if (haveBefore) {
					int64_t newUs =
						(int64_t)tspec.tv_sec * 1000000 +
						tspec.tv_nsec / 1000;
					int64_t oldUs =
						(int64_t)before.tv_sec * 1000000 +
						before.tv_nsec / 1000;

					print_clock_delta(newUs - oldUs);
				}
				failures = 0;
				write_rtc_from_system();
			}
		}

		/* Returns 0 when woken by ntp_wake(), -EAGAIN on timeout. */
		if (k_sem_take(&ntp_wake_sem, K_SECONDS(wait_sec)) == 0) {
			/* Say what the wake leads to: with NTP off the next round
			 * only idles, so a plain "resyncing now" would be wrong. */
			if (ntp_is_enabled()) {
				LOG_INF("wake received, resyncing now");
			} else {
				LOG_INF("wake received, NTP is off, "
					"staying idle");
			}
			/* The user changed the address or the switch, so the
			 * next failures deserve to be reported again. */
			failures = 0;
		}
	}
}

/* 16384, not 8192: the deepest path is socket receive inside sntp_query(),
 * which runs the network stack on this thread's stack. A fault dump showed sp
 * 2880 bytes below an 8192-byte stack, so the old size was too small; the
 * start-up restore over overlayfs adds to the same budget. */
K_THREAD_DEFINE(ntp_sync_tid, 16384, ntp_sync_thread, NULL, NULL, NULL, 7, 0, 0);
