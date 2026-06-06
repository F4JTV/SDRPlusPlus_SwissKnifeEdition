cmake_minimum_required(VERSION 3.13)
project(tetra_demodulator)

# ---------------------------------------------------------------------------
# Source files (identical in both build modes).
#
# We deliberately split the source list in two:
#   * SRC          : files always present in the repo (DSP, decoder glue,
#                    lower_mac, phy, crypto). Globbed because the lists are
#                    long and stable.
#   * ETSI_CODEC_SRC : files supplied by the ETSI reference codec, which the
#                    user has to download via download_and_patch.sh BEFORE
#                    running cmake. Listed explicitly so that a missing
#                    codec triggers a clear cmake-time error instead of
#                    silently producing a .so with undefined symbols.
# ---------------------------------------------------------------------------
file(GLOB SRC LIST_DIRECTORIES false
    "src/*.cpp"
    "src/*.c"
    "src/dsp/*.cpp"
    "src/decoder/src/*.cpp"
    "src/decoder/src/*.c"
    "src/decoder/src/crypto/*.cpp"
    "src/decoder/src/crypto/*.c"
    "src/decoder/src/lower_mac/*.cpp"
    "src/decoder/src/lower_mac/*.c"
    "src/decoder/src/phy/*.cpp"
    "src/decoder/src/phy/*.c"
)

set(ETSI_CODEC_SRC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/decoder/codec/c-code/cdec_tet.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/decoder/codec/c-code/sub_cd.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/decoder/codec/c-code/tetra_op.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/decoder/codec/c-code/sdec_tet.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/decoder/codec/c-code/sub_sc_d.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/decoder/codec/c-code/sub_dsp.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/decoder/codec/c-code/fbas_tet.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/decoder/codec/c-code/fexp_tet.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/decoder/codec/c-code/fmat_tet.c"
)

foreach (f ${ETSI_CODEC_SRC})
    if (NOT EXISTS "${f}")
        message(FATAL_ERROR
            "\n"
            "tetra_demodulator: missing ETSI codec source file:\n"
            "    ${f}\n"
            "\n"
            "This package is shipped with the ETSI TETRA speech codec already\n"
            "extracted and patched under src/decoder/codec/. If that directory\n"
            "is missing, you probably deleted or moved it by accident.\n"
            "\n"
            "To recreate it, run from the module's root directory:\n"
            "\n"
            "    cd src/decoder/etsi_codec-patches\n"
            "    ./download_and_patch.sh       # needs network access to ETSI\n"
            "    cd -\n"
            "\n"
            "Then DELETE THIS BUILD DIRECTORY and re-run cmake (a stale CMake\n"
            "cache will not pick up the newly extracted files automatically).\n"
        )
    endif ()
endforeach ()

list(APPEND SRC ${ETSI_CODEC_SRC})

# ---------------------------------------------------------------------------
# Decide build mode:
#   * IN-TREE  : being built from SDR++ source tree (sdrpp_core target exists)
#   * OUT-OF-TREE: standalone build, uses sdrpp_module.cmake macro
# ---------------------------------------------------------------------------
if (TARGET sdrpp_core)
    message(STATUS "tetra_demodulator: building IN-TREE")

    add_library(${PROJECT_NAME} SHARED ${SRC})
    target_link_libraries(${PROJECT_NAME} PRIVATE sdrpp_core)
    set_target_properties(${PROJECT_NAME} PROPERTIES PREFIX "")

    if (MSVC)
        target_compile_options(${PROJECT_NAME} PRIVATE
            /O2 /Ob2 $<$<COMPILE_LANGUAGE:CXX>:/std:c++17> /EHsc)
    elseif (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(${PROJECT_NAME} PRIVATE
            -O3
            $<$<COMPILE_LANGUAGE:C>:-std=c11>
            $<$<COMPILE_LANGUAGE:CXX>:-std=c++17>
            -Wno-unused-command-line-argument -undefined dynamic_lookup)
    else ()
        target_compile_options(${PROJECT_NAME} PRIVATE
            -O3
            $<$<COMPILE_LANGUAGE:C>:-std=c11>
            $<$<COMPILE_LANGUAGE:CXX>:-std=c++17>)
    endif ()

    install(TARGETS ${PROJECT_NAME} DESTINATION lib/sdrpp/plugins)
else ()
    message(STATUS "tetra_demodulator: building OUT-OF-TREE")
    set(CMAKE_INSTALL_PREFIX "/usr/")
    include(GNUInstallDirs)

    if (NOT SDRPP_MODULE_CMAKE)
        set(SDRPP_MODULE_CMAKE "/usr/share/cmake/Modules/sdrpp_module.cmake")
    endif ()
    include(${SDRPP_MODULE_CMAKE})
endif ()

# ---------------------------------------------------------------------------
# Threading: the data exporter spawns a worker thread for TCP I/O, so we
# need pthread in both build modes. Use the standard CMake Threads package
# which handles -lpthread / -pthread / Win32 transparently.
# ---------------------------------------------------------------------------
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE Threads::Threads)

# ---------------------------------------------------------------------------
# Per-target C tuning. -march=native is applied only to OUR target so we never
# leak it to other modules built alongside this one in the SDR++ tree.
# ---------------------------------------------------------------------------
if (NOT MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE
        $<$<COMPILE_LANGUAGE:C>:-march=native>)
endif ()

# ---------------------------------------------------------------------------
# Common include directories (apply in both modes)
# ---------------------------------------------------------------------------
target_include_directories(${PROJECT_NAME} PRIVATE BEFORE
    "src/"
    "src/decoder/src"
    "src/decoder/codec"
)
