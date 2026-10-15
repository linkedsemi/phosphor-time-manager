#include <errno.h>
#include <stdio.h>

#include <zephyr/device.h>
#include <zephyr/drivers/rtc.h>
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

/* Address set from the web UI / Redfish, written by ntp_config.cpp through
 * ntp_set_user_server(). Empty means unset, and ntp_server above is used.
 *
 * Kept in this C file rather than ntp_config.cpp so K_MUTEX_DEFINE stays in C,
 * where its designated initialiser is less compiler-sensitive. */
static char userServer[NTP_USER_SERVER_MAX];
static K_MUTEX_DEFINE(userServerLock);

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

void ntp_set_user_server(const char *server)
{
	k_mutex_lock(&userServerLock, K_FOREVER);
	if (server == NULL || server[0] == '\0') {
		userServer[0] = '\0';
	} else {
		snprintf(userServer, sizeof(userServer), "%s", server);
	}
	k_mutex_unlock(&userServerLock);

	printk("[ntp] user server <- %s\n",
	       (server == NULL || server[0] == '\0') ? "(cleared)" : server);

	ntp_wake();
}

/* Copy the user address out. Returns true when one is set and buf is valid. */
static bool user_server_snapshot(char *buf, size_t len)
{
	bool found = false;

	if (len == 0) {
		return false;
	}

	k_mutex_lock(&userServerLock, K_FOREVER);
	if (userServer[0] != '\0') {
		snprintf(buf, len, "%s", userServer);
		found = true;
	}
	k_mutex_unlock(&userServerLock);

	return found;
}

#define NTP_TIMEOUT_MS   3000 /* per-query timeout */
#define NTP_RESYNC_SEC   3600 /* interval after a successful sync */
#define NTP_RETRY_SEC    10   /* interval after a failure */
#define NTP_NET_WAIT_SEC 5    /* network poll interval */

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

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	/* Nothing to do without a default server. */
	if (sizeof(ntp_server) == 1) {
		printk("[ntp] NTP_SERVER not configured, disabled\n");
		return;
	}

	printk("[ntp] default server %s:%d\n", ntp_server,
	       CONFIG_OPENBMC_PHOSPHOR_TIME_MANAGER_NTP_PORT);

	for (;;) {
		uint32_t wait_sec = NTP_RESYNC_SEC;
		char userbuf[NTP_USER_SERVER_MAX];
		const char *host;
		bool from_user;
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

		/* The user address wins when set; otherwise the Kconfig default. */
		from_user = user_server_snapshot(userbuf, sizeof(userbuf));
		host = from_user ? userbuf : ntp_server;

		rc = query_once(host, &ts);

		/* User server unreachable -- try the built-in address once. */
		if (rc != 0 && from_user) {
			printk("[ntp] user server %s unreachable, fallback to %s\n",
			       host, ntp_server);
			host = ntp_server;
			rc = query_once(host, &ts);
		}

		if (rc != 0) {
			printk("[ntp] query failed, retry in %ds\n", NTP_RETRY_SEC);
			wait_sec = NTP_RETRY_SEC;
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
				write_rtc_from_system();
			}
		}

		/* Returns 0 when woken by ntp_wake(), -EAGAIN on timeout. */
		if (k_sem_take(&ntp_wake_sem, K_SECONDS(wait_sec)) == 0) {
			printk("[ntp] woken, resync now\n");
		}
	}
}

K_THREAD_DEFINE(ntp_sync_tid, 4096, ntp_sync_thread, NULL, NULL, NULL, 7, 0, 0);
