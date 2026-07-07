cmake_minimum_required(VERSION 3.20)
project(restos_ledger CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

option(RESTOS_SANITIZE "Enable ASan/UBSan (Linux/clang/gcc)" OFF)
option(RESTOS_TSAN "Enable ThreadSanitizer (Linux)" OFF)
if(RESTOS_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=address,undefined)
endif()
if(RESTOS_TSAN)
  add_compile_options(-fsanitize=thread -g)
  add_link_options(-fsanitize=thread)
endif()

option(RESTOS_WERROR "Treat warnings as errors (CI lint gate)" OFF)
if(MSVC)
  add_compile_options(/W4 /permissive-)
  if(RESTOS_WERROR)
    add_compile_options(/WX)
  endif()
else()
  add_compile_options(-Wall -Wextra -Wpedantic -Wno-missing-field-initializers)
  if(RESTOS_WERROR)
    add_compile_options(-Werror)
  endif()
endif()

# Core library (pure logic — no sockets, deterministically testable)
add_library(restos_core
  src/log.cpp
  src/kv.cpp
  src/raft.cpp
)
target_include_directories(restos_core PUBLIC include)

# Networking + executables
add_library(restos_net src/net.cpp)
target_include_directories(restos_net PUBLIC include)
target_link_libraries(restos_net PUBLIC restos_core)
if(WIN32)
  target_link_libraries(restos_net PUBLIC ws2_32)
endif()

add_executable(restos-node src/node_main.cpp)
target_link_libraries(restos-node PRIVATE restos_net)

add_executable(restos-cli src/cli_main.cpp)
target_link_libraries(restos-cli PRIVATE restos_net)

# Tests
enable_testing()
add_subdirectory(tests)
