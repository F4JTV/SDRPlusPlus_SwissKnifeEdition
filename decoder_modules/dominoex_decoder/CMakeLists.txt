cmake_minimum_required(VERSION 3.13)
project(dominoex_decoder)

file(GLOB SRC "src/*.cpp")

include(${SDRPP_MODULE_CMAKE})

target_include_directories(dominoex_decoder PRIVATE "src/")
