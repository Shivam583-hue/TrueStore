cmake_minimum_required(VERSION 3.13)
project(TrueStore)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED True)
set(THREADS_PREFER_PTHREAD_FLAG ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)

add_library(truestore_core STATIC
  src/aof/aof.cpp
  src/auth/auth.cpp
  src/pubsub/pubsub.cpp
  src/replication/replication.cpp
  src/config/config.cpp
  src/server/server.cpp
  src/net/socket.cpp
  src/rdb/rdb.cpp
  src/resp/resp.cpp
  src/store/store.cpp
  src/store/strings.cpp
  src/store/lists.cpp
  src/store/streams.cpp
  src/store/config_commands.cpp
  src/store/sorted_sets.cpp
  src/store/geospatial.cpp
  src/store/bitmaps.cpp
  src/command/command.cpp
  src/stream/stream.cpp
  third_party/rax/rax.c
)
target_include_directories(truestore_core PUBLIC src third_party)
target_link_libraries(truestore_core PUBLIC Threads::Threads OpenSSL::Crypto)

add_executable(server src/main.cpp)
target_link_libraries(server PRIVATE truestore_core)

add_executable(client
  src/client_main.cpp
  src/client/client.cpp
)
target_include_directories(client PRIVATE src)
target_link_libraries(client PRIVATE Threads::Threads)
