cmake_minimum_required(VERSION 3.13)
project(satellite_tracker)

# Module sources: the C++ module + the vendored libpredict (C) sources.
file(GLOB SRC "src/*.cpp" "src/predict/*.c")

include(${SDRPP_MODULE_CMAKE})

target_include_directories(satellite_tracker PRIVATE "src/")

# libpredict is C; make sure those .c files build as C99.
set_source_files_properties(
    ${CMAKE_CURRENT_SOURCE_DIR}/src/predict/julian_date.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/predict/moon.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/predict/observer.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/predict/orbit.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/predict/refraction.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/predict/sdp4.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/predict/sgp4.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/predict/sun.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/predict/unsorted.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/predict/version.c
    PROPERTIES LANGUAGE C
)

# libcurl for TLE downloads (CelesTrak is HTTPS-only).
find_package(CURL REQUIRED)
target_link_libraries(satellite_tracker PRIVATE CURL::libcurl)
