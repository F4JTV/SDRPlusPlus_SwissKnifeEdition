cmake_minimum_required(VERSION 3.13)
project(freedv_decoder)

file(GLOB_RECURSE SRC "src/*.cpp" "src/*.c")
# Exclude the standalone generator / decode test harness from the module build.
list(FILTER SRC EXCLUDE REGEX "src/test/.*")

include(${SDRPP_MODULE_CMAKE})

target_include_directories(freedv_decoder PRIVATE "src/")

# FreeDV decoding is provided by libcodec2 (the freedv_api.h API), the same
# dependency the M17 decoder uses.
if (MSVC)
    target_include_directories(freedv_decoder PRIVATE "C:/Program Files/codec2/include/")
    target_link_directories(freedv_decoder PRIVATE "C:/Program Files/codec2/lib")
    target_link_libraries(freedv_decoder PRIVATE libcodec2)
elseif (ANDROID)
    target_include_directories(freedv_decoder PUBLIC
        /sdr-kit/${ANDROID_ABI}/include/codec2
    )
    target_link_libraries(freedv_decoder PUBLIC
        /sdr-kit/${ANDROID_ABI}/lib/libcodec2.so
    )
else ()
    find_package(PkgConfig)
    pkg_check_modules(LIBCODEC2 REQUIRED codec2)

    target_include_directories(freedv_decoder PRIVATE ${LIBCODEC2_INCLUDE_DIRS})
    target_link_directories(freedv_decoder PRIVATE ${LIBCODEC2_LIBRARY_DIRS})
    target_link_libraries(freedv_decoder PRIVATE ${LIBCODEC2_LIBRARIES})

    if (${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
        target_include_directories(freedv_decoder PRIVATE "/usr/local/include")
    endif()
endif ()
