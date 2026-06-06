cmake_minimum_required(VERSION 3.13)
project(gps_decoder)

# Gather sources BEFORE including the SDR++ module helper: the helper uses the
# ${SRC} variable to create the shared library target.
file(GLOB_RECURSE SRC "src/*.cpp" "src/*.c")

# This include creates the target add_library(${PROJECT_NAME} SHARED ${SRC}),
# links it to sdrpp_core, sets PREFIX "", applies the module compiler flags and
# installs into lib/sdrpp/plugins. Do NOT call add_library yourself.
include(${SDRPP_MODULE_CMAKE})

target_include_directories(gps_decoder PRIVATE "src/")

# --- FFTW3 (single-precision) for the parallel code-phase acquisition --------
# Prefer pkg-config on Linux/macOS, fall back to a manual find on Windows where
# FFTW is typically installed via vcpkg or PothosSDR.
if (MSVC)
    find_path(FFTW3F_INCLUDE_DIR fftw3.h)
    find_library(FFTW3F_LIBRARY NAMES fftw3f libfftw3f-3 fftw3f-3)
    target_include_directories(gps_decoder PRIVATE ${FFTW3F_INCLUDE_DIR})
    target_link_libraries(gps_decoder PRIVATE ${FFTW3F_LIBRARY})
elseif (ANDROID)
    target_include_directories(gps_decoder PUBLIC
        /sdr-kit/${ANDROID_ABI}/include
    )
    target_link_libraries(gps_decoder PUBLIC
        /sdr-kit/${ANDROID_ABI}/lib/libfftw3f.so
    )
else ()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FFTW3F REQUIRED fftw3f)
    target_include_directories(gps_decoder PRIVATE ${FFTW3F_INCLUDE_DIRS})
    target_link_directories(gps_decoder PRIVATE ${FFTW3F_LIBRARY_DIRS})
    target_link_libraries(gps_decoder PRIVATE ${FFTW3F_LIBRARIES})
endif ()
