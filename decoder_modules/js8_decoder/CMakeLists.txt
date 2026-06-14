cmake_minimum_required(VERSION 3.13)
project(js8_decoder)

file(GLOB SRC "src/*.cpp")

if (NOT SDRPP_MODULE_CMAKE)
    message(FATAL_ERROR
        "SDRPP_MODULE_CMAKE is not set. Build this module from inside the "
        "SDR++ source tree (place it in decoder_modules/ and enable "
        "OPT_BUILD_JS8_DECODER), not standalone.")
endif()
include(${SDRPP_MODULE_CMAKE})

target_include_directories(js8_decoder PRIVATE "src/")
set(CMAKE_CXX_STANDARD 17)
