cmake_minimum_required(VERSION 3.16)
project(wangpan_server CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 每个任务新增的 .cpp 都要加进这个列表，不要遗漏。
set(APP_SOURCES
  src/main.cpp
  src/config.cpp
  src/db/db.cpp
  src/db/users.cpp
  src/db/sessions.cpp
  src/db/files.cpp
)

add_executable(wangpan_server ${APP_SOURCES})
target_include_directories(wangpan_server PRIVATE src third_party)
find_package(SQLite3 REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)
target_link_libraries(wangpan_server PRIVATE SQLite::SQLite3 OpenSSL::Crypto Threads::Threads)

enable_testing()
add_executable(test_schema tests/test_schema.cpp src/db/db.cpp src/db/users.cpp)
target_include_directories(test_schema PRIVATE src third_party)
target_link_libraries(test_schema PRIVATE SQLite::SQLite3 Threads::Threads)
add_test(NAME schema COMMAND test_schema)
