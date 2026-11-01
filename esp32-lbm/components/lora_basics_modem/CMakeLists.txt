# ESP-IDF component: Semtech LoRa Basics Modem (SWL2001) on SX126x.
#
# The upstream library is built from third_party/SWL2001 without its own CMake
# project (it calls project() and install(), which do not fit a component). The
# source lists and compile definitions below mirror lbm_lib/smtc_modem_core/
# CMakeLists.txt and the options in lbm_lib/options.cmake.

# ESP-IDF reads this file twice. The first pass (CMAKE_BUILD_EARLY_EXPANSION) only
# collects component dependencies, and CONFIG_* options are not defined yet, so
# only the dependency lists may be used there.
if(CMAKE_BUILD_EARLY_EXPANSION)
    idf_component_register(
        REQUIRES esp_timer nvs_flash
        PRIV_REQUIRES driver esp_rom esp_system freertos log
    )
    return()
endif()

set(LBM_ROOT "${CMAKE_CURRENT_LIST_DIR}/third_party/SWL2001/lbm_lib")
if(NOT EXISTS "${LBM_ROOT}/smtc_modem_core/smtc_modem.c")
    message(FATAL_ERROR "LoRa Basics Modem sources not found at ${LBM_ROOT}")
endif()
set(CORE "${LBM_ROOT}/smtc_modem_core")
set(DRV  "${CORE}/radio_drivers/sx126x_driver/src")

# ------------------------------------------------------------------ sources --

set(LBM_PORT_SRCS
    src/lbm_esp32.c
    src/smtc_modem_hal_esp32.c
    src/sx126x_hal_esp32.c
    src/ral_sx126x_bsp_esp32.c
)

set(LBM_UPSTREAM_SRCS
    # radio driver, RAL, RALF
    ${DRV}/sx126x.c
    ${DRV}/sx126x_driver_version.c
    ${DRV}/sx126x_lr_fhss.c
    ${DRV}/lr_fhss_mac.c
    ${CORE}/smtc_ral/src/ral_sx126x.c
    ${CORE}/smtc_ralf/src/ralf_sx126x.c
    # crypto (software secure element)
    ${CORE}/smtc_modem_crypto/smtc_modem_crypto.c
    ${CORE}/smtc_modem_crypto/soft_secure_element/aes.c
    ${CORE}/smtc_modem_crypto/soft_secure_element/cmac.c
    ${CORE}/smtc_modem_crypto/soft_secure_element/soft_se.c
    # core
    ${CORE}/lorawan_api/lorawan_api.c
    ${CORE}/lorawan_manager/lorawan_cid_request_management.c
    ${CORE}/lorawan_manager/lorawan_dwn_ack_management.c
    ${CORE}/lorawan_manager/lorawan_join_management.c
    ${CORE}/lorawan_manager/lorawan_send_management.c
    ${CORE}/lorawan_packages/lorawan_certification/lorawan_certification.c
    ${CORE}/lr1mac/src/lr1_stack_mac_layer.c
    ${CORE}/lr1mac/src/lr1mac_core.c
    ${CORE}/lr1mac/src/lr1mac_utilities.c
    ${CORE}/lr1mac/src/services/smtc_duty_cycle.c
    ${CORE}/lr1mac/src/services/smtc_lbt.c
    ${CORE}/modem_supervisor/modem_supervisor_light.c
    ${CORE}/modem_supervisor/modem_tx_protocol_manager.c
    ${CORE}/modem_utilities/fifo_ctrl.c
    ${CORE}/modem_utilities/modem_core.c
    ${CORE}/modem_utilities/modem_event_utilities.c
    ${CORE}/radio_planner/src/radio_planner.c
    ${CORE}/smtc_modem_test.c
    ${CORE}/smtc_modem.c
    # regional parameters
    ${CORE}/lr1mac/src/smtc_real/src/smtc_real.c
)

set(LBM_DEFS
    SX126X
    NUMBER_OF_STACKS=1
    RP2_103
)

if(CONFIG_LBM_RADIO_SX1261)
    list(APPEND LBM_DEFS SX1261)
elseif(CONFIG_LBM_RADIO_SX1268)
    list(APPEND LBM_DEFS SX1268)
else()
    list(APPEND LBM_DEFS SX1262)
endif()

if(CONFIG_LBM_MODEM_TRACE)
    list(APPEND LBM_DEFS MODEM_HAL_DBG_TRACE=1)
else()
    list(APPEND LBM_DEFS MODEM_HAL_DBG_TRACE=0)
endif()
if(CONFIG_LBM_MODEM_TRACE AND CONFIG_LBM_MODEM_TRACE_DEEP)
    list(APPEND LBM_DEFS MODEM_HAL_DEEP_DBG_TRACE=1)
else()
    list(APPEND LBM_DEFS MODEM_HAL_DEEP_DBG_TRACE=0)
endif()

# Regions: each one adds a source file and a REGION_<name> define.
set(LBM_REGION_LIST "")
if(CONFIG_LBM_REGION_EU868)
    list(APPEND LBM_REGION_LIST EU_868)
endif()
if(CONFIG_LBM_REGION_US915)
    list(APPEND LBM_REGION_LIST US_915)
endif()
if(CONFIG_LBM_REGION_AU915)
    list(APPEND LBM_REGION_LIST AU_915)
endif()
if(CONFIG_LBM_REGION_AS923)
    list(APPEND LBM_REGION_LIST AS_923)
endif()
if(CONFIG_LBM_REGION_CN470)
    list(APPEND LBM_REGION_LIST CN_470)
endif()
if(CONFIG_LBM_REGION_CN470_RP_1_0)
    list(APPEND LBM_REGION_LIST CN_470_RP_1_0)
endif()
if(CONFIG_LBM_REGION_IN865)
    list(APPEND LBM_REGION_LIST IN_865)
endif()
if(CONFIG_LBM_REGION_KR920)
    list(APPEND LBM_REGION_LIST KR_920)
endif()
if(CONFIG_LBM_REGION_RU864)
    list(APPEND LBM_REGION_LIST RU_864)
endif()
if(NOT LBM_REGION_LIST)
    message(FATAL_ERROR "LoRa Basics Modem: enable at least one region in menuconfig")
endif()
foreach(region IN LISTS LBM_REGION_LIST)
    string(TOLOWER "${region}" region_lower)
    list(APPEND LBM_UPSTREAM_SRCS ${CORE}/lr1mac/src/smtc_real/src/region_${region_lower}.c)
    list(APPEND LBM_DEFS REGION_${region})
endforeach()

set(LBM_CORE_INCLUDES
    ${LBM_ROOT}
    ${CORE}
    ${CORE}/modem_supervisor
    ${CORE}/modem_utilities
    ${CORE}/lorawan_packages/lorawan_certification
    ${CORE}/lorawan_manager
    ${CORE}/lorawan_api
    ${CORE}/lr1mac
    ${CORE}/lr1mac/src
    ${CORE}/lr1mac/src/services
    ${CORE}/lr1mac/src/smtc_real/src
    ${CORE}/radio_planner/src
    ${CORE}/smtc_modem_crypto
    ${CORE}/smtc_modem_crypto/smtc_secure_element
    ${CORE}/smtc_ral/src
    ${CORE}/smtc_ralf/src
    ${DRV}
)

if(CONFIG_LBM_CLASS_B)
    list(APPEND LBM_DEFS ADD_CLASS_B)
    list(APPEND LBM_CORE_INCLUDES ${CORE}/lr1mac/src/lr1mac_class_b)
    list(APPEND LBM_UPSTREAM_SRCS
        ${CORE}/lorawan_manager/lorawan_class_b_management.c
        ${CORE}/lr1mac/src/lr1mac_class_b/smtc_beacon_sniff.c
        ${CORE}/lr1mac/src/lr1mac_class_b/smtc_ping_slot.c
    )
endif()
if(CONFIG_LBM_CLASS_C)
    list(APPEND LBM_DEFS ADD_CLASS_C)
    list(APPEND LBM_CORE_INCLUDES ${CORE}/lr1mac/src/lr1mac_class_c)
    list(APPEND LBM_UPSTREAM_SRCS ${CORE}/lr1mac/src/lr1mac_class_c/lr1mac_class_c.c)
endif()
if(CONFIG_LBM_CSMA)
    list(APPEND LBM_DEFS ADD_CSMA)
    list(APPEND LBM_UPSTREAM_SRCS ${CORE}/lr1mac/src/services/smtc_lora_cad_bt.c)
endif()

# ------------------------------------------------------------- registration --

idf_component_register(
    SRCS ${LBM_PORT_SRCS} ${LBM_UPSTREAM_SRCS}
    INCLUDE_DIRS
        include
        ${LBM_ROOT}/smtc_modem_api
        ${LBM_ROOT}/smtc_modem_hal
        ${CORE}/logging
    PRIV_INCLUDE_DIRS
        src
        ${LBM_CORE_INCLUDES}
    REQUIRES
        esp_timer
        nvs_flash
    PRIV_REQUIRES
        driver
        esp_rom
        esp_system
        freertos
        log
)

target_compile_definitions(${COMPONENT_LIB} PUBLIC ${LBM_DEFS})

# Upstream sources are built as shipped; do not let ESP-IDF's -Werror stop the
# build on warnings in code we do not own. Warnings are still printed.
set_source_files_properties(${LBM_UPSTREAM_SRCS} PROPERTIES COMPILE_OPTIONS "-Wno-error")
