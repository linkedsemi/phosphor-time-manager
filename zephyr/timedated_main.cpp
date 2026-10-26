/* org.freedesktop.timedate1 for Zephyr, replacing systemd-timedated.
 *
 * That service comes from systemd on a normal OpenBMC, and systemd is not
 * available here, so a minimal stand-in is implemented instead.
 *
 * Callers:
 *   bmcweb (web UI / Redfish) -- SetTime, SetNTP, and the NTP property
 *   phosphor-host-ipmid       -- reads the NTP property before Set SEL Time
 *
 * Split with ntp_time.c: this file sets the time and toggles NTP; ntp_time.c
 * is the SNTP client. It reads the switch state through ntp_is_enabled().
 */
#include <dbus_broker.h>
#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <sdbusplus/exception.hpp>
#include <sdbusplus/vtable.hpp>
#include <zephyr/device.h>
#include <zephyr/drivers/rtc.h>
#include <zephyr/kernel.h>
#include <zephyr/posix/time.h>
#include <zephyr/sys/printk.h>

#include <boost/asio/io_context.hpp>

#include <atomic>
#include <memory>
#include <string>
#include <variant>

/* Defined in app/src/task_def.cpp */
extern struct k_sem timedated_ready_sem;

/* Defined in ntp_config.cpp. Registers the minimal xyz.openbmc_project.Network
 * service on the same connection, so no extra thread is needed and no app-side
 * change is required. */
void ntp_config_register(sdbusplus::asio::connection& bus,
			 sdbusplus::asio::object_server& server);

/* Defined in ntp_time.c. Restarts the sync thread immediately; needed when the
 * NTP switch is toggled, otherwise the thread sleeps for a full resync period.
 * The other wake path is a server address change, in ntp_set_user_server(). */
extern "C" void ntp_wake(void);

/* NTP switch state. Read and written over D-Bus by the web UI / Redfish, and
 * read by the ntp_time.c sync thread. Enabled by default. */
static std::atomic<bool> ntpEnabled{true};

/* Given when the switch state is first known: either the boot restore (the
 * persisted mode reaches manager.cpp, which calls SetNTP) or a user toggle.
 * ntp_time.c waits on it before its first round, because the default above is
 * "on" and the persisted mode only arrives about 15 s into boot. */
K_SEM_DEFINE(ntp_mode_known, 0, 1);

extern "C" bool ntp_is_enabled(void)
{
	return ntpEnabled.load();
}

/* Accepted range for SetTime. The bounds are the limits of the IPMI SEL
 * timestamp field, which is a uint32 count of seconds since 1970; values
 * outside it are truncated in SEL into a plausible-looking but wrong date.
 * The same bounds are used in app/src/rtc_time.c and ntp_time.c.
 *
 * Any time within the range is accepted. SetTime writes the hardware RTC,
 * which cannot be undone, so out-of-range values are rejected. */
#define MIN_VALID_EPOCH 0LL          /* 1970-01-01, Unix epoch */
#define MAX_VALID_EPOCH 4294967295LL /* 2106-02-07, uint32 limit */

/* Write the current system time to the hardware RTC.
 *
 * Duplicates ntp_time.c and rtc_shell.c; the three copies should be merged
 * into a single helper in app/src/rtc_time.c. */
static void write_rtc_from_system(void)
{
	const struct device *rtc = DEVICE_DT_GET(DT_NODELABEL(rtc));
	struct rtc_time rtc_tm = {0};
	struct timespec ts;
	struct tm tm;

	if (!device_is_ready(rtc)) {
		printk("[timedated] rtc not ready, skip\n");
		return;
	}
	if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
		printk("[timedated] clock_gettime failed\n");
		return;
	}
	if (gmtime_r(&ts.tv_sec, &tm) == NULL) {
		printk("[timedated] gmtime_r failed\n");
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
		printk("[timedated] rtc_set_time failed\n");
		return;
	}

	printk("[timedated] RTC <- %d-%02u-%02u %02u:%02u:%02u UTC\n",
	       tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
	       tm.tm_hour, tm.tm_min, tm.tm_sec);
}

/* Mirror the switch into the setting phosphor-time-manager reads at boot, so
 * that turning NTP off survives a reboot. settings-manager is what persists it
 * (its directory is on /overlay, see the mako template).
 *
 * Writing this fires PropertiesChanged, which manager.cpp turns back into a
 * SetNTP call. The callers only persist when the value actually changed, which
 * is what stops that from looping. */
static void persist_ntp_mode(sdbusplus::asio::connection& conn, bool enabled)
{
	constexpr const char* settingPath =
		"/xyz/openbmc_project/time/sync_method";
	constexpr const char* settingIntf =
		"xyz.openbmc_project.Time.Synchronization";

	static const std::string ntpMode =
		"xyz.openbmc_project.Time.Synchronization.Method.NTP";
	static const std::string manualMode =
		"xyz.openbmc_project.Time.Synchronization.Method.Manual";

	try
	{
		std::variant<std::string> mode(enabled ? ntpMode : manualMode);
		auto method = conn.new_method_call(
			"xyz.openbmc_project.Settings", settingPath,
			"org.freedesktop.DBus.Properties", "Set");
		method.append(settingIntf, "TimeSyncMethod", mode);
		conn.call(method);
	}
	catch (const std::exception& e)
	{
		printk("[timedated] cannot save the NTP mode: %s\n", e.what());
	}
}

/* systemd semantics: while NTP is on, a manually set time would be overwritten
 * by the next sync, so the request is refused instead. bmcweb maps this error
 * name to a Redfish propertyValueConflict. */
static void throwAutomaticTimeSyncEnabled(void)
{
	sd_bus_error err = SD_BUS_ERROR_MAKE_CONST(
		"org.freedesktop.timedate1.AutomaticTimeSyncEnabled",
		"Automatic time synchronization is enabled");
	throw sdbusplus::exception::SdBusError(&err, "SetTime");
}

/* Initialisation. bus and server are returned by reference because they must
 * outlive io.run() -- a destructed object_server emits InterfacesRemoved and
 * the service disappears.
 *
 * Every step from make_shared() through initialize() can throw. An exception
 * escaping the thread function would either be caught by task_trampoline
 * (thread dies, ready semaphore never given, dependents wait for a timeout) or
 * unwind past the C frames and abort the image. The caller wraps this in try. */
static void timedated_setup(boost::asio::io_context& io, sd_bus* sdbus,
			    std::shared_ptr<sdbusplus::asio::connection>& bus,
			    std::shared_ptr<sdbusplus::asio::object_server>& server)
{
	bus = std::make_shared<sdbusplus::asio::connection>(io, sdbus);

	bus->request_name("org.freedesktop.timedate1");

	server = std::make_shared<sdbusplus::asio::object_server>(bus);
	auto iface = server->add_interface("/org/freedesktop/timedate1",
					   "org.freedesktop.timedate1");

	/* A second service name on the same connection (see ntp_config.cpp). */
	ntp_config_register(*bus, *server);

	/* NTP property: the switch shown by the web UI. */
	iface->register_property_rw<bool>(
		"NTP", ntpEnabled.load(),
		sdbusplus::vtable::property_::emits_change,
		[bus](const bool& req, const bool& /*cur*/) {
			/* Only speak when the value really changes: persist
			 * writes the setting back, which bounces through
			 * manager.cpp into a second SetNTP for the same
			 * value. */
			if (ntpEnabled.exchange(req) != req)
			{
				persist_ntp_mode(*bus, req);
				printk("[timedated] NTP %s\n",
				       req ? "on, will sync from a server"
					   : "off, time is manual");
			}
			k_sem_give(&ntp_mode_known);
			/* Wake the sync thread so the new state takes effect
			 * now instead of after a full resync period. */
			ntp_wake();
			return true;
		},
		[](const bool& /*cur*/) { return ntpEnabled.load(); });

	/* SetNTP(bool ntpEnabled, bool user_interaction) -- same as above. */
	iface->register_method(
		"SetNTP",
		[bus](const bool& enabled, const bool& /*userInteraction*/) {
			if (ntpEnabled.exchange(enabled) != enabled)
			{
				persist_ntp_mode(*bus, enabled);
				/* Plain wording rather than the raw D-Bus
				 * argument: SetNTP(false) on its own reads
				 * like "sync now". */
				printk("[timedated] NTP %s\n",
				       enabled ? "on, will sync from a server"
					       : "off, time is manual");
			}
			k_sem_give(&ntp_mode_known);
			ntp_wake();
		});

	/* SetTime(int64 usec, bool relative, bool user_interaction).
	 *
	 * The first parameter must be int64_t (D-Bus signature "x"), not
	 * uint64_t ("t"): bmcweb passes
	 * std::chrono::duration<int64_t, micro>::count(), and sd-bus compares
	 * the signature strictly before dispatching, replying InvalidArgs
	 * without entering this lambda (bus-objects.c method_callbacks_run). */
	iface->register_method(
		"SetTime",
		[](const int64_t& usec, const bool& relative,
		   const bool& /*userInteraction*/) {
			struct timespec ts;

			if (ntpEnabled.load()) {
				throwAutomaticTimeSyncEnabled();
			}
			if (relative) {
				/* Not used by bmcweb, which sends false.
				 * Rejected rather than silently treated as
				 * an absolute time. */
				printk("[timedated] SetTime: relative not supported\n");
				throw sdbusplus::exception::SdBusError(
					-EINVAL, "SetTime relative");
			}
			if (usec < MIN_VALID_EPOCH * 1000000LL ||
			    usec > MAX_VALID_EPOCH * 1000000LL) {
				printk("[timedated] refuse SetTime %lld (out of range)\n",
				       (long long)usec);
				throw sdbusplus::exception::SdBusError(
					-ERANGE, "SetTime out of range");
			}

			ts.tv_sec = (time_t)(usec / 1000000LL);
			ts.tv_nsec = (long)((usec % 1000000LL) * 1000LL);

			if (clock_settime(CLOCK_REALTIME, &ts) != 0) {
				printk("[timedated] clock_settime failed\n");
				throw sdbusplus::exception::SdBusError(
					-EIO, "clock_settime");
			}

			printk("[timedated] SetTime -> %lld\n",
			       (long long)ts.tv_sec);
			write_rtc_from_system();
		});

	iface->initialize();
}

int timedated_main()
{
	boost::asio::io_context io;

	sd_bus* sdbus = nullptr;
	if (connect_to_dbroker(&sdbus) < 0) {
		printk("[timedated] failed to connect to dbroker\n");
		k_sem_give(&timedated_ready_sem);
		return -1;
	}

	std::shared_ptr<sdbusplus::asio::connection> bus;
	std::shared_ptr<sdbusplus::asio::object_server> server;

	try
	{
		timedated_setup(io, sdbus, bus, server);
	}
	catch (const std::exception& e)
	{
		/* Log the reason, but still give the semaphore: dependents
		 * should come up and discover the service is unavailable
		 * themselves, rather than time out waiting for it. */
		printk("[timedated] init failed: %s\n", e.what());
	}

	k_sem_give(&timedated_ready_sem);

	io.run();
	return 0;
}
