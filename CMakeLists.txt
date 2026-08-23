cmake_minimum_required(VERSION 3.13)
project(TrueStore)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED True)
set(THREADS_PREFER_PTHREAD_FLAG ON)

find_package(Threads REQUIRED)

add_executable(server
  src/main.cpp
  src/server/server.cpp
)
target_include_directories(server PRIVATE src)
target_link_libraries(server PRIVATE Threads::Threads)

add_executable(client
  src/client_main.cpp
  src/client/client.cpp
)
target_include_directories(client PRIVATE src)
target_link_libraries(client PRIVATE Threads::Threads)
