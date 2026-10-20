#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/rtc.h>
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/sntp.h>
#include <zephyr/net/socket.h>
#include <zephyr/posix/time.h>
#include <zephyr/sys/printk.h>

/* ntp_set_user_server() is defined below and called from ntp_config.cpp. */
#include "ntp_config.h"

/* From zephyr/Kconfig. sizeof("") == 1, so the array size tells at compile
 * time whether a default server was configured. This is the built-in address:
 * it is used when the user has not set one, or when theirs is unreachable. */
static const char ntp_server[] = CONFIG_OPENBMC_PHOSPHOR_TIME_MANAGER_NTP_SERVER;

/* Defined in timedated_main.cpp. While disabled this thread does not sync, so
 * a manually set time is not overwritten. */
extern bool ntp_is_enabled(void);

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
		printk("[ntp] cannot save the server list; keeping the previous "
		       "one\n");
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
		printk("[ntp] cannot save the server list (%d); removed the saved "
		       "one so the built-in server is used after a reboot\n",
		       closed);
		printk("[ntp] set the servers again in the web UI (Settings -> "
		       "Date & Time) to retry saving them\n");
	}
}

/* Set once the file has been looked at, so the sync thread and the property
 * initialiser can both ask for it without racing or re-reading. */
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

	if (userServersLoaded) {
		return;
	}
	userServersLoaded = true;

	fs_file_t_init(&f);

	rc = fs_open(&f, NTP_USER_SERVER_PATH, FS_O_READ);
	if (rc != 0) {
		/* -ENOENT just means nothing has been saved yet. */
		if (rc != -ENOENT) {
			printk("[ntp] cannot read the saved server list (%d)\n",
			       rc);
		}
		return;
	}
	n = fs_read(&f, buf, sizeof(buf) - 1);
	fs_close(&f);
	if (n < 0) {
		printk("[ntp] cannot read the saved server list (%d)\n", (int)n);
		return;
	}
	if (n == 0) {
		return; /* empty file */
	}
	buf[n] = '\0';

	k_mutex_lock(&userServerLock, K_FOREVER);

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
			printk("[ntp] restored server %s\n", userServer[count]);
			count++;
		}

		if (eol == NULL) {
			break;
		}
		line = eol + 1;
	}

	userServerCount = count;
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

	printk("[ntp] user servers <-");
	if (n == 0) {
		printk(" (cleared)\n");
	} else {
		for (i = 0; i < n; i++) {
			printk(" %s", snapshot[i]);
		}
		printk("\n");
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
		printk("[ntp] cannot resolve '%s' (getaddrinfo %d)\n", host, rc);
		return -EINVAL;
	}

	*out = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
	zsock_freeaddrinfo(res);

	printk("[ntp] resolved %s -> %s\n", host,
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
		printk("[ntp] sntp_init failed\n");
		return -EIO;
	}

	rc = sntp_query(&ctx, NTP_TIMEOUT_MS, out);
	sntp_close(&ctx);

	return rc;
}

/* Write the current system time to the hardware RTC.
 *
 * Duplicates rtc_shell.c and timedated_main.cpp; the three copies should be
 * merged into a single helper in app/src/rtc_time.c. */
static void write_rtc_from_system(void)
{
	const struct device *rtc = DEVICE_DT_GET(DT_NODELABEL(rtc));
	struct rtc_time rtc_tm = {0};
	struct timespec ts;
	struct tm tm;

	if (!device_is_ready(rtc)) {
		printk("[ntp] rtc not ready, skip\n");
		return;
	}
	if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
		printk("[ntp] clock_gettime failed\n");
		return;
	}
	if (gmtime_r(&ts.tv_sec, &tm) == NULL) {
		printk("[ntp] gmtime_r failed\n");
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
		printk("[ntp] rtc_set_time failed\n");
		return;
	}

	printk("[ntp] RTC <- %d-%02u-%02u %02u:%02u:%02u UTC\n",
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
			printk("[ntp] NTP_SERVER not configured, disabled\n");
			return;
		}
		printk("[ntp] no default server, using the saved list\n");
	} else {
		printk("[ntp] default server %s:%d\n", ntp_server,
		       CONFIG_OPENBMC_PHOSPHOR_TIME_MANAGER_NTP_PORT);
	}

	for (;;) {
		uint32_t wait_sec = NTP_RESYNC_SEC;
		char userbuf[NTP_MAX_SERVERS][NTP_USER_SERVER_MAX];
		const char *host;
		int n_user;
		int i;
		int rc;

		/* Skip while NTP is switched off (manual mode). */
		if (!ntp_is_enabled()) {
			k_sleep(K_SECONDS(NTP_NET_WAIT_SEC));
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
			rc = query_once(userbuf[i], &ts);
			if (rc == 0) {
				host = userbuf[i];
				break;
			}
			printk("[ntp] %s unreachable (%d)\n", userbuf[i], rc);
		}

		/* Only worth falling back when a built-in server exists. */
		if (rc != 0 && sizeof(ntp_server) > 1) {
			if (n_user > 0) {
				printk("[ntp] no user server answered, fallback to %s\n",
				       ntp_server);
			}
			host = ntp_server;
			rc = query_once(host, &ts);
		}

		if (rc != 0) {
			failures++;
			if (failures <= NTP_LOUD_FAILURES) {
				printk("[ntp] query failed (%d/%d), retry in %ds\n",
				       failures, NTP_LOUD_FAILURES, NTP_RETRY_SEC);
				wait_sec = NTP_RETRY_SEC;
			} else {
				if (failures == NTP_LOUD_FAILURES + 1) {
					printk("[ntp] no NTP server is reachable; "
					       "switching to slow mode and retrying "
					       "every %d min\n",
					       NTP_SLOW_RETRY_SEC / 60);
					printk("[ntp] set a server in the web UI "
					       "(Settings -> Date & Time) to sync "
					       "right away\n");
				}
				wait_sec = NTP_SLOW_RETRY_SEC;
			}
		} else if (ts.seconds < NTP_MIN_VALID_EPOCH ||
			   ts.seconds > NTP_MAX_VALID_EPOCH) {
			/* Implausible value: no system time, and no RTC write. */
			printk("[ntp] implausible %llu from %s, ignored\n",
			       (unsigned long long)ts.seconds, host);
			wait_sec = NTP_RETRY_SEC;
		} else if (!ntp_is_enabled()) {
			/* NTP was switched off while the query was in flight.
			 * Re-checked here because the check at the top of the
			 * loop happened before the query. */
			printk("[ntp] disabled during query, skip write\n");
			wait_sec = NTP_RETRY_SEC;
		} else {
			tspec.tv_sec = (time_t)ts.seconds;
			tspec.tv_nsec =
				((uint64_t)ts.fraction * NSEC_PER_SEC) >> 32;

			if (clock_settime(CLOCK_REALTIME, &tspec) != 0) {
				printk("[ntp] clock_settime failed\n");
				wait_sec = NTP_RETRY_SEC;
			} else {
				printk("[ntp] synced to %llu via %s\n",
				       (unsigned long long)ts.seconds, host);
				failures = 0;
				write_rtc_from_system();
			}
		}

		/* Returns 0 when woken by ntp_wake(), -EAGAIN on timeout. */
		if (k_sem_take(&ntp_wake_sem, K_SECONDS(wait_sec)) == 0) {
			printk("[ntp] woken, resync now\n");
			/* The user changed the address or the switch, so the
			 * next failures deserve to be reported again. */
			failures = 0;
		}
	}
}

/* 8192, not 4096: the deepest path is the start-up restore, which runs on top
 * of overlayfs's own 256-byte path buffers. Measured 3388 of 4096 in use after
 * persistence was added, leaving too little room for anything new. */
K_THREAD_DEFINE(ntp_sync_tid, 8192, ntp_sync_thread, NULL, NULL, NULL, 7, 0, 0);
