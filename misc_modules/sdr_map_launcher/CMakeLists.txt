cmake_minimum_required(VERSION 3.13)
project(sdr_map_launcher)

# Module sources
file(GLOB SRC "src/*.cpp")

# SDR++ provides this helper when sources are dropped into decoder_modules/
# (in-tree build). It also wires the include paths to core/, gui/, signal_path/.
if (MSVC)
    set(CMAKE_CXX_FLAGS "/O2 /Ob2 /std:c++17 /EHsc")
else ()
    set(CMAKE_CXX_FLAGS "-O3 -std=c++17 -fpermissive -Wno-unused-command-line-argument")
endif ()

include(${SDRPP_MODULE_CMAKE})
