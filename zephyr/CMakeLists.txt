if(CONFIG_OPENBMC_PHOSPHOR_TIME_MANAGER)
    zephyr_library_named(phosphor-time-manager)
    zephyr_library_sources(
        ${ZEPHYR_CURRENT_MODULE_DIR}/bmc_epoch.cpp
        ${ZEPHYR_CURRENT_MODULE_DIR}/manager.cpp
        ${ZEPHYR_CURRENT_MODULE_DIR}/settings.cpp
        ${ZEPHYR_CURRENT_MODULE_DIR}/utils.cpp
        ${ZEPHYR_CURRENT_MODULE_DIR}/main.cpp
        ${ZEPHYR_CURRENT_MODULE_DIR}/zephyr/timedated_main.cpp
        ${ZEPHYR_CURRENT_MODULE_DIR}/zephyr/ntp_config.cpp
        ${ZEPHYR_CURRENT_MODULE_DIR}/zephyr/ntp_time.c
    )

    zephyr_library_compile_options(
        -include ${ZEPHYR_BASE}/include/zephyr/logging/log.h
    )

    zephyr_library_link_libraries(basu)
    zephyr_library_link_libraries(sdbusplus)
    zephyr_library_link_libraries(phosphor-logging)
    zephyr_library_link_libraries(phosphor-dbus-interfaces)
    zephyr_library_link_libraries(boost)

    zephyr_library_include_directories(${ZEPHYR_CURRENT_MODULE_DIR})
    zephyr_library_include_directories(${ZEPHYR_CURRENT_MODULE_DIR}/zephyr)
    # connect_to_dbroker() / dbus_broker.h come from the main app; the Zephyr
    # branch of main.cpp uses them.
    zephyr_include_directories(${ZEPHYR_BASE}/../openbmc/openbmc_zephyr_app/app/src)
endif()