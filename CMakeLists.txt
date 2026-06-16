cmake_minimum_required(VERSION 3.10...3.19)
project(nvmkv)

# 开启优化 
# set(CMAKE_BUILD_TYPE Release)
# set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2")
# set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mavx2")
# set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mavx512f")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=native")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -pthread")

add_subdirectory(ert)
add_subdirectory(fastalloc)
add_subdirectory(rng)
add_subdirectory(roert)
# add_subdirectory(fastfair)
# add_subdirectory(lbtree)
# add_subdirectory(wope)
add_subdirectory(wort)
add_subdirectory(woart)
add_subdirectory(roart)

# add_executable(nvmkv mulit_thread_main.cpp)
# add_executable(nvmkv db_recovery.cpp)
add_executable(nvmkv db_bench.cpp)

target_link_libraries(nvmkv PRIVATE
nvmkv-roert nvmkv-ert nvmkv-wort nvmkv-woart nvmkv-roart nvmkv-rng nvmkv-fastalloc)


