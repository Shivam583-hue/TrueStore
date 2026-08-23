cmake_minimum_required(VERSION 3.13)
project(TrueStore)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED True)
set(THREADS_PREFER_PTHREAD_FLAG ON)

file(GLOB_RECURSE SOURCE_FILES src/*.cpp)

find_package(Threads REQUIRED)

find_path(ASIO_INCLUDE_DIR NAMES asio.hpp)

add_executable(server ${SOURCE_FILES})

target_include_directories(server PRIVATE ${ASIO_INCLUDE_DIR})

target_link_libraries(server PRIVATE Threads::Threads)
