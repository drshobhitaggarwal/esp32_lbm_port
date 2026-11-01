cmake_minimum_required(VERSION 3.16)

# Pick up the lora_basics_modem component from this repository.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(lbm_periodic_uplink)
