cmake_minimum_required(VERSION 3.24)
project(lacc LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(LACC_STATIC "Link the guest programs statically (they run in a busybox initramfs)" OFF)
option(LACC_TESTS "Build the host unit tests" ON)
option(LACC_SANITIZE "Build the host unit tests with ASan and UBSan" OFF)

add_compile_options(-Wall -Wextra -Werror -Wno-missing-field-initializers)

add_library(lacc_runtime STATIC runtime/src/device.cpp)
target_include_directories(lacc_runtime PUBLIC runtime/include include)

add_executable(lacc-validate validate/lacc_validate.cpp)
target_link_libraries(lacc-validate PRIVATE lacc_runtime)

add_executable(lacc-mlp examples/mlp.cpp)
target_link_libraries(lacc-mlp PRIVATE lacc_runtime)

find_package(Threads REQUIRED)
target_link_libraries(lacc-validate PRIVATE Threads::Threads)
if(LACC_STATIC)
  target_link_options(lacc-validate PRIVATE -static)
  target_link_options(lacc-mlp PRIVATE -static)
endif()

if(LACC_TESTS)
  find_package(GTest REQUIRED)        # libgtest-dev
  find_package(ZLIB REQUIRED)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)

  add_executable(lacc_tests runtime/tests/test_lns.cpp runtime/tests/test_fw.cpp runtime/tests/test_mlp.cpp)
  target_include_directories(lacc_tests PRIVATE runtime/include include examples)
  target_link_libraries(lacc_tests PRIVATE GTest::gtest_main ZLIB::ZLIB)
  target_compile_definitions(lacc_tests PRIVATE LACC_MKFW="${CMAKE_SOURCE_DIR}/fw/mkfw.py"
                                                LACC_PYTHON="${Python3_EXECUTABLE}")
  if(LACC_SANITIZE)
    target_compile_options(lacc_tests PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all)
    target_link_options(lacc_tests PRIVATE -fsanitize=address,undefined)
  endif()
  enable_testing()
  include(GoogleTest)
  gtest_discover_tests(lacc_tests)
endif()
