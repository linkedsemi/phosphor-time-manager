/* NTP server list set from the web UI / Redfish -- C/C++ interface.
 *
 * The list and its lock live in ntp_time.c (the only reader). This header
 * exposes the accessors, called by ntp_config.cpp. Plain C types only, so
 * ntp_time.c can include it directly.
 */
#ifndef NTP_CONFIG_H_
#define NTP_CONFIG_H_

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum length of a single address, including the terminating '\0'. */
#define NTP_USER_SERVER_MAX 64

/* Most addresses kept. The web UI offers three (primary / secondary /
 * tertiary); the sync thread tries them in order and only falls back to the
 * Kconfig default when none of them answers. */
#define NTP_MAX_SERVERS 3

/* Replace the list (implemented in ntp_time.c). count == 0 clears it, falling
 * back to the Kconfig default. NULL or empty entries are skipped. */
void ntp_set_user_servers(const char *const *servers, int count);

/* Copy the current list out, one address per slot, and return how many were
 * written (never more than max). ntp_config.cpp uses it to initialise
 * StaticNTPServers from what was restored from disk, so the page shows what is
 * actually in use rather than an empty list. */
int ntp_get_user_servers(char out[][NTP_USER_SERVER_MAX], int max);

#ifdef __cplusplus
}
#endif

#endif /* NTP_CONFIG_H_ */
