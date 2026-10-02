cmake_minimum_required(VERSION 3.24)
project(SpaceLedger VERSION 0.2.0 LANGUAGES CXX)

if(NOT WIN32 OR NOT MSVC)
  message(FATAL_ERROR "SpaceLedger currently requires Windows and the MSVC toolchain.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(spaceledger_core
  src/windows.cpp
  src/scanner.cpp
  src/database.cpp
  src/history.cpp
  src/diff.cpp)
target_include_directories(spaceledger_core PUBLIC include)
target_compile_definitions(spaceledger_core PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
target_link_libraries(spaceledger_core PUBLIC winsqlite3)
target_compile_options(spaceledger_core PUBLIC /EHsc)
target_compile_options(spaceledger_core PRIVATE /W4 /permissive- /utf-8)

add_executable(spaceledger src/main.cpp)
target_link_libraries(spaceledger PRIVATE spaceledger_core)
target_compile_options(spaceledger PRIVATE /W4 /permissive- /utf-8)

include(CTest)
if(BUILD_TESTING)
  add_executable(spaceledger_tests tests/tests.cpp)
  target_link_libraries(spaceledger_tests PRIVATE spaceledger_core advapi32)
  target_compile_options(spaceledger_tests PRIVATE /W4 /permissive- /utf-8)
  add_test(NAME spaceledger_core_and_ntfs COMMAND spaceledger_tests)
  add_test(NAME spaceledger_cli_help COMMAND spaceledger --help)
  add_test(NAME spaceledger_cli_workflow COMMAND ${CMAKE_COMMAND}
    "-DSPACELEDGER_EXE=$<TARGET_FILE:spaceledger>" -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_workflow.cmake")
  add_test(NAME spaceledger_scheduled_runner COMMAND ${CMAKE_COMMAND}
    "-DSPACELEDGER_EXE=$<TARGET_FILE:spaceledger>"
    "-DSCHEDULED_RUNNER=${CMAKE_CURRENT_SOURCE_DIR}/tools/run-scheduled.ps1"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/scheduled_runner.cmake")
endif()
