/* NTP server address set from the web UI / Redfish -- C/C++ interface.
 *
 * The value and its lock live in ntp_time.c (the only reader). This header
 * exposes just the write entry point, called by ntp_config.cpp. Plain C types
 * only, so ntp_time.c can include it directly.
 */
#ifndef NTP_CONFIG_H_
#define NTP_CONFIG_H_

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum length of a user-supplied address, including the terminating '\0'. */
#define NTP_USER_SERVER_MAX 64

/* Set a new NTP server address (implemented in ntp_time.c).
 * NULL or an empty string clears it, falling back to the Kconfig default. */
void ntp_set_user_server(const char *server);

#ifdef __cplusplus
}
#endif

#endif /* NTP_CONFIG_H_ */
