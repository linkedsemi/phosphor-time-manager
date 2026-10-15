#include "config.h"

#include "bmc_epoch.hpp"
#include "manager.hpp"

#include <sdbusplus/bus.hpp>

#ifdef __ZEPHYR__
/* There is no Linux system bus socket here: dbus-broker is reached over an
 * AF_UNIX socketpair via connect_to_dbroker(), and the event loop is asio
 * rather than sd-event. Follows phosphor-sel-logger/src/sel_logger.cpp. */
#include <dbus_broker.h>
#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <zephyr/kernel.h>

#include <boost/asio/io_context.hpp>

/* Defined in app/src/task_def.cpp; given once initialisation is done, which
 * releases the threads that depend on this one. */
extern struct k_sem time_manager_ready_sem;

int time_manager_main()
#else
int main()
#endif
{
#ifdef __ZEPHYR__
    /* Every step here (connection ctor, manager_t, Manager, BmcEpoch,
     * request_name) can throw. An exception escaping the thread function
     * would either be caught by task_trampoline (thread dies, ready semaphore
     * never given, dependents wait for a timeout) or unwind past the C frames
     * and abort the image. Hence the try, and the semaphore is given even on
     * failure.
     *
     * The body is deliberately left unindented so both #ifdef branches stay
     * close to upstream. */
    try
    {
    boost::asio::io_context io;

    sd_bus* sdbus = nullptr;
    if (connect_to_dbroker(&sdbus) < 0)
    {
        lg2::error("time_manager: failed to connect to dbroker");
        k_sem_give(&time_manager_ready_sem);
        return -1;
    }
    sdbusplus::asio::connection bus(io, sdbus);
#else
    auto bus = sdbusplus::bus::new_default();
    sd_event* event = nullptr;

    auto eventDeleter = [](sd_event* e) { sd_event_unref(e); };
    using SdEvent = std::unique_ptr<sd_event, decltype(eventDeleter)>;

    // acquire a reference to the default event loop
    sd_event_default(&event);
    SdEvent sdEvent{event, eventDeleter};
    event = nullptr;

    // attach bus to this event loop
    bus.attach_event(sdEvent.get(), SD_EVENT_PRIORITY_NORMAL);
#endif
    // Add sdbusplus ObjectManager
    sdbusplus::server::manager_t bmcEpochObjManager(bus, objpathBmc);

    phosphor::time::Manager manager(bus);
    phosphor::time::BmcEpoch bmc(bus, objpathBmc, manager);

    bus.request_name(busname);

#ifdef __ZEPHYR__
    k_sem_give(&time_manager_ready_sem);

    io.run();
    }
    catch (const std::exception& e)
    {
        lg2::error("time_manager: init failed: {ERROR}", "ERROR", e.what());
        k_sem_give(&time_manager_ready_sem);
        return -1;
    }
#else
    // Start event loop for all sd-bus events and timer event
    sd_event_loop(bus.get_event());

    bus.detach_event();
#endif

    return 0;
}
