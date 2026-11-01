# Host build of the component against mock ESP-IDF headers. This checks that the component's
# CMakeLists lists real files, every upstream symbol resolves (whole-archive link), the port
# compiles with strict warnings, and the port's own logic behaves. It is not an ESP-IDF build.
cmake_minimum_required(VERSION 3.16)
project(lbm_host_check C)

set(COMP_DIR "${CMAKE_CURRENT_LIST_DIR}/../../components/lora_basics_modem")
set(EXAMPLE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../examples/periodic_uplink")
set(SDKCONFIG_DIR "" CACHE PATH "Directory holding the generated sdkconfig.h and sdkconfig.cmake")
set(TIMER_ISR ON CACHE BOOL "Pretend CONFIG_ESP_TIMER_SUPPORT_ISR_DISPATCH_METHOD=y")

include(${SDKCONFIG_DIR}/sdkconfig.cmake)

set(COMPONENT_LIB lora_basics_modem_lib)
function(idf_component_register)
    cmake_parse_arguments(A "" "" "SRCS;INCLUDE_DIRS;PRIV_INCLUDE_DIRS;REQUIRES;PRIV_REQUIRES" ${ARGN})
    set(srcs "")
    foreach(s IN LISTS A_SRCS)
        if(NOT IS_ABSOLUTE "${s}")
            set(s "${COMP_DIR}/${s}")
        endif()
        if(NOT EXISTS "${s}")
            message(FATAL_ERROR "component lists a source that does not exist: ${s}")
        endif()
        list(APPEND srcs "${s}")
    endforeach()
    add_library(${COMPONENT_LIB} STATIC ${srcs})
    foreach(d IN LISTS A_INCLUDE_DIRS)
        if(NOT IS_ABSOLUTE "${d}")
            set(d "${COMP_DIR}/${d}")
        endif()
        if(NOT IS_DIRECTORY "${d}")
            message(FATAL_ERROR "component lists an include dir that does not exist: ${d}")
        endif()
        target_include_directories(${COMPONENT_LIB} PUBLIC "${d}")
    endforeach()
    foreach(d IN LISTS A_PRIV_INCLUDE_DIRS)
        if(NOT IS_ABSOLUTE "${d}")
            set(d "${COMP_DIR}/${d}")
        endif()
        if(NOT IS_DIRECTORY "${d}")
            message(FATAL_ERROR "component lists a private include dir that does not exist: ${d}")
        endif()
        target_include_directories(${COMPONENT_LIB} PRIVATE "${d}")
    endforeach()
endfunction()

include(${COMP_DIR}/CMakeLists.txt)

include_directories(${CMAKE_CURRENT_LIST_DIR}/mock ${SDKCONFIG_DIR})
target_include_directories(${COMPONENT_LIB} PRIVATE ${CMAKE_CURRENT_LIST_DIR}/mock ${SDKCONFIG_DIR})
if(TIMER_ISR)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC CONFIG_ESP_TIMER_SUPPORT_ISR_DISPATCH_METHOD=1)
else()
    target_compile_definitions(${COMPONENT_LIB} PUBLIC CONFIG_ESP_TIMER_SUPPORT_ISR_DISPATCH_METHOD=0)
endif()

# The port's own files get ESP-IDF-style strictness; upstream files keep the component's -Wno-error.
get_target_property(all_srcs ${COMPONENT_LIB} SOURCES)
foreach(s IN LISTS all_srcs)
    if(s MATCHES "/components/lora_basics_modem/src/")
        set_source_files_properties(${s} PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra;-Werror;-Wno-unused-parameter")
    endif()
endforeach()

# Example app: compile main.c against the same headers.
add_library(example_obj OBJECT ${EXAMPLE_DIR}/main/main.c)
target_include_directories(example_obj PRIVATE ${EXAMPLE_DIR}/main)
target_link_libraries(example_obj PRIVATE ${COMPONENT_LIB})
target_compile_options(example_obj PRIVATE -Wall -Wextra -Werror -Wno-unused-parameter)

add_executable(host_test test_main.c mock_impl.c)
target_include_directories(host_test PRIVATE ${COMP_DIR}/src
    ${COMP_DIR}/third_party/SWL2001/lbm_lib/smtc_modem_core/radio_drivers/sx126x_driver/src
    ${COMP_DIR}/third_party/SWL2001/lbm_lib/smtc_modem_core/smtc_ral/src)
target_link_libraries(host_test PRIVATE
    -Wl,--whole-archive ${COMPONENT_LIB} -Wl,--no-whole-archive
    $<TARGET_OBJECTS:example_obj>)
target_compile_options(host_test PRIVATE -Wall -Wextra -Wno-unused-parameter)
