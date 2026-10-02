cmake_minimum_required(VERSION 3.24)
string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef fixture_id)
set(fixture "${CMAKE_CURRENT_BINARY_DIR}/spaceledger-scheduled-${fixture_id}")
file(MAKE_DIRECTORY "${fixture}/data")
file(WRITE "${fixture}/data/example.txt" "scheduled data")
set(database "${fixture}/history.db")
set(shell "$ENV{SystemRoot}/System32/WindowsPowerShell/v1.0/powershell.exe")
if(NOT EXISTS "${shell}")
  message(FATAL_ERROR "Windows PowerShell 5.1 was not found: ${shell}")
endif()

foreach(repetition RANGE 1 3)
  execute_process(COMMAND "${shell}" -NoProfile -NonInteractive -ExecutionPolicy Bypass
    -File "${SCHEDULED_RUNNER}" -Executable "${SPACELEDGER_EXE}"
    -Root "${fixture}/data" -Database "${database}" -Keep 2
    RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE errors ENCODING UTF-8)
  if(NOT "${status}" STREQUAL "0")
    message(FATAL_ERROR "Scheduled run ${repetition} returned ${status}:\n${output}\n${errors}")
  endif()
endforeach()

execute_process(COMMAND "${SPACELEDGER_EXE}" scans --database "${database}"
  RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE errors ENCODING UTF-8)
if(NOT "${status}" STREQUAL "0" OR output MATCHES "1  " OR NOT output MATCHES "2  " OR NOT output MATCHES "3  ")
  message(FATAL_ERROR "Scheduled retention did not keep exactly the newest two snapshots:\n${output}\n${errors}")
endif()
message(STATUS "Scheduled runner saved and pruned snapshots; fixtures: ${fixture}")
