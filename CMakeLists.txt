cmake_minimum_required(VERSION 3.15)
project(FlashKV CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Extensions stay on: the networking layer uses POSIX APIs (localtime_r,
# MSG_NOSIGNAL, sockets) that strict -std=c++20 hides behind __STRICT_ANSI__.
set(CMAKE_CXX_EXTENSIONS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -pedantic -O2)

find_package(Threads REQUIRED)

enable_testing()

add_subdirectory(src)
add_subdirectory(tests)
