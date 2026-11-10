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
  src/db/blobs.cpp
  src/storage/blob.cpp
  src/util/crypto.cpp
  src/api/auth.cpp
  src/api/files.cpp
  src/api/manage.cpp
  src/api/upload.cpp
  src/api/download.cpp
  src/api/preview.cpp
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

add_executable(test_auth tests/test_auth.cpp
  src/db/db.cpp src/db/users.cpp src/db/sessions.cpp src/util/crypto.cpp src/api/auth.cpp)
target_include_directories(test_auth PRIVATE src third_party)
target_link_libraries(test_auth PRIVATE SQLite::SQLite3 OpenSSL::Crypto Threads::Threads)
add_test(NAME auth COMMAND test_auth)

add_executable(test_storage tests/test_storage.cpp src/storage/blob.cpp src/util/crypto.cpp)
target_include_directories(test_storage PRIVATE src third_party)
target_link_libraries(test_storage PRIVATE OpenSSL::Crypto)
add_test(NAME storage COMMAND test_storage)

add_executable(test_instant tests/test_instant.cpp
  src/db/db.cpp src/db/users.cpp src/db/blobs.cpp src/db/files.cpp)
target_include_directories(test_instant PRIVATE src third_party)
target_link_libraries(test_instant PRIVATE SQLite::SQLite3 Threads::Threads)
add_test(NAME instant COMMAND test_instant)

add_executable(test_range tests/test_range.cpp)
target_include_directories(test_range PRIVATE src third_party)
add_test(NAME range COMMAND test_range)

add_executable(test_trash tests/test_trash.cpp
  src/db/db.cpp src/db/users.cpp src/db/blobs.cpp src/db/files.cpp)
target_include_directories(test_trash PRIVATE src third_party)
target_link_libraries(test_trash PRIVATE SQLite::SQLite3 Threads::Threads)
add_test(NAME trash COMMAND test_trash)
