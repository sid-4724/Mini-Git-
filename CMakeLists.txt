cmake_minimum_required(VERSION 3.16)
project(MiniGit VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(minigit_core
    src/storage.cpp
    src/repository.cpp
    src/worktree.cpp
    src/diff.cpp
    src/graph.cpp
    src/merge.cpp
)
target_include_directories(minigit_core PUBLIC include)
target_include_directories(minigit_core SYSTEM PRIVATE third_party)
if(MSVC)
    target_compile_options(minigit_core PRIVATE /W4)
else()
    target_compile_options(minigit_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(minigit src/main.cpp)
target_link_libraries(minigit PRIVATE minigit_core)

include(CTest)
if(BUILD_TESTING)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME cli.storage_staging
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cli.py $<TARGET_FILE:minigit> StorageStagingTests)
    add_test(NAME cli.history_diff
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cli.py $<TARGET_FILE:minigit> HistoryDiffTests)
    add_test(NAME cli.branches
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cli.py $<TARGET_FILE:minigit> BranchTests)
    add_test(NAME cli.merges
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cli.py $<TARGET_FILE:minigit> MergeTests)
    add_test(NAME cli.dag
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cli.py $<TARGET_FILE:minigit> DagTests)
    add_test(NAME cli.safety
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cli.py $<TARGET_FILE:minigit> SafetyTests)
    add_test(NAME cli.recovery
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cli.py $<TARGET_FILE:minigit> RecoveryTests)
    add_test(NAME cli.integration
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cli.py $<TARGET_FILE:minigit> IntegrationTests)
    add_executable(minigit_unit_tests tests/unit_tests.cpp)
    target_link_libraries(minigit_unit_tests PRIVATE minigit_core)
    add_test(NAME algorithms_and_codec COMMAND minigit_unit_tests)
endif()
