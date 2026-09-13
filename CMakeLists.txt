cmake_minimum_required(VERSION 3.13)
project(TrueStore)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED True)
set(THREADS_PREFER_PTHREAD_FLAG ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Threads REQUIRED)

add_executable(server
  src/main.cpp
  src/server/server.cpp
  src/net/socket.cpp
  src/resp/resp.cpp
  src/store/store.cpp
  src/store/strings.cpp
  src/store/lists.cpp
  src/store/streams.cpp
  src/store/replication.cpp
  src/store/config.cpp
  src/command/command.cpp
  src/stream/stream.cpp
  third_party/rax/rax.c
)
target_include_directories(server PRIVATE src third_party)
target_link_libraries(server PRIVATE Threads::Threads)

add_executable(client
  src/client_main.cpp
  src/client/client.cpp
)
target_include_directories(client PRIVATE src)
target_link_libraries(client PRIVATE Threads::Threads)
