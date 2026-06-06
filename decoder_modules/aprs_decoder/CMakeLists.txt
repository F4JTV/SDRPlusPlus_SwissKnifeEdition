cmake_minimum_required(VERSION 3.13)
project(aprs_decoder)

file(GLOB_RECURSE SRC "src/*.cpp" "src/*.c")

include(${SDRPP_MODULE_CMAKE})

target_include_directories(aprs_decoder PRIVATE "src/")

# Install the APRS symbol sheets (aprs.fi set, CC BY-SA 4.0) so the decoded
# packets table can show the symbol icons. They land in the SDR++ resources
# directory under "aprs/" (i.e. <resourcesDirectory>/aprs/aprs-symbols-64-*.png).
# Linux/Unix install prefix maps <resourcesDirectory> to share/sdrpp.
if (NOT (WIN32 OR APPLE))
    install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/resources/aprs DESTINATION share/sdrpp)
endif ()

