/* Minimal xyz.openbmc_project.Network, only so the web UI can store an NTP
 * server address.
 *
 * bmcweb reads and writes it as Network.EthernetInterface.StaticNTPServers on
 * /xyz/openbmc_project/network/eth0 (see network_protocol.hpp). Without the
 * service, that page returns an error and the address field cannot be saved.
 *
 * Upstream this service comes from phosphor-networkd, which manages the whole
 * network configuration. Only the one property is needed here, so IP / DHCP /
 * routing are left to app/src/net_stack_cfg.c. If phosphor-networkd is ported
 * later, delete this file and drop the ntp_config_register() call from
 * timedated_main.cpp (the service name would conflict).
 *
 * It shares the timedated connection and thread, which avoids a new thread and
 * any app-side changes. The address itself is handed to ntp_time.c.
 */
#include "ntp_config.h"

#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <sdbusplus/vtable.hpp>
#include <zephyr/sys/printk.h>

#include <memory>
#include <string>
#include <vector>

/* Called from timedated_main.cpp; runs on the timedated io_context thread.
 *
 * Reuses the caller's bus and object_server rather than creating its own: a
 * second object_server would register another ObjectManager at "/" on the same
 * connection. The caller keeps them alive for the process lifetime. */
void ntp_config_register(sdbusplus::asio::connection& bus,
			 sdbusplus::asio::object_server& server)
{
	/* Handled locally instead of propagating: a failure here must not stop
	 * timedated from initialising the timedate1 interface afterwards.
	 * request_name() is the likely thrower -- it raises when the name is
	 * already taken, which would happen if phosphor-networkd is ever ported. */
	try
	{
		bus.request_name("xyz.openbmc_project.Network");

		/* An object manager has to be registered at this exact path.
		 * sd-bus only answers GetManagedObjects when one is registered on
		 * the queried path (bus-objects.c process_get_managed_objects),
		 * and object_server only adds one for "/" by itself. */
		server.add_manager("/xyz/openbmc_project/network");

		auto iface = server.add_interface(
			"/xyz/openbmc_project/network/eth0",
			"xyz.openbmc_project.Network.EthernetInterface");

		iface->register_property_rw<std::vector<std::string>>(
			"StaticNTPServers", std::vector<std::string>{},
			sdbusplus::vtable::property_::emits_change,
			[](const std::vector<std::string>& req,
			   std::vector<std::string>& cur) {
				/* Only the first address is used; there is a
				 * single sync thread. */
				ntp_set_user_server(
					req.empty() ? nullptr : req[0].c_str());

				cur = req;
				return true;
			},
			[](const std::vector<std::string>& cur) { return cur; });

		/* Read by bmcweb as well. Empty arrays are enough: there is no
		 * DHCP-supplied NTP server and no domain name configured. */
		iface->register_property("NTPServers",
					 std::vector<std::string>{});
		iface->register_property("DomainName",
					 std::vector<std::string>{});

		iface->initialize();

		printk("[ntpcfg] xyz.openbmc_project.Network up\n");
	}
	catch (const std::exception& e)
	{
		printk("[ntpcfg] failed to register "
		       "xyz.openbmc_project.Network: %s\n",
		       e.what());
	}
}
