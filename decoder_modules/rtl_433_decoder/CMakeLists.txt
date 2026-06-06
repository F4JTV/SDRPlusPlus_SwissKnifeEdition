cmake_minimum_required(VERSION 3.13)
project(rtl_433_bridge)

file(GLOB_RECURSE SRC "src/*.cpp" "src/*.c")

include(${SDRPP_MODULE_CMAKE})

target_include_directories(rtl_433_bridge PRIVATE "src/")

# --- rtl_433 library ------------------------------------------------------
# The bridge links against rtl_433's static library (libr_433.a) and uses its
# public + private headers (r_api.h, r_private.h, baseband.h, ...).
#
# Provide the rtl_433 source/build location via -DRTL_433_ROOT=/path/to/rtl_433
# (its build dir must contain src/libr_433.a, produced with:
#   cmake -DENABLE_RTLSDR=OFF -DENABLE_SOAPYSDR=OFF .. && make r_433 )
#
# If RTL_433_ROOT is not set, we try pkg-config for a system install.

if (DEFINED RTL_433_ROOT)
    message(STATUS "rtl_433_bridge: using RTL_433_ROOT=${RTL_433_ROOT}")
    target_include_directories(rtl_433_bridge PRIVATE
        "${RTL_433_ROOT}/include"
        "${RTL_433_ROOT}/src")
    find_library(RTL_433_LIB
        NAMES r_433 libr_433
        PATHS "${RTL_433_ROOT}/build/src" "${RTL_433_ROOT}/build"
        NO_DEFAULT_PATH)
    if (NOT RTL_433_LIB)
        message(FATAL_ERROR "rtl_433_bridge: libr_433.a not found under ${RTL_433_ROOT}/build. "
                            "Build it first: cd ${RTL_433_ROOT} && mkdir -p build && cd build && "
                            "cmake -DENABLE_RTLSDR=OFF -DENABLE_SOAPYSDR=OFF .. && make r_433")
    endif ()
    target_link_libraries(rtl_433_bridge PRIVATE ${RTL_433_LIB})
else ()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(RTL_433 librtl_433)
    if (RTL_433_FOUND)
        message(STATUS "rtl_433_bridge: using pkg-config librtl_433")
        target_include_directories(rtl_433_bridge PRIVATE ${RTL_433_INCLUDE_DIRS})
        target_link_libraries(rtl_433_bridge PRIVATE ${RTL_433_LIBRARIES})
    else ()
        message(FATAL_ERROR "rtl_433_bridge: rtl_433 not found. Pass -DRTL_433_ROOT=/path/to/rtl_433 "
                            "(with build/src/libr_433.a present) or install librtl_433.")
    endif ()
endif ()

# rtl_433 needs the math and pthread libs.
find_package(Threads REQUIRED)
target_link_libraries(rtl_433_bridge PRIVATE m Threads::Threads)
