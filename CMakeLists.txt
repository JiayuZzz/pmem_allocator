cmake_minimum_required(VERSION 3.12.4)
project(PMEM_ALLOCATOR
        DESCRIPTION "PMEM ALLOCATOR POC"
        LANGUAGES CXX C)

set(SOURCES src/pmem_allocator.cpp
        src/free_list.cpp
        src/thread_manager.cpp)

add_library(pmem_allocator SHARED ${SOURCES})
target_link_libraries(pmem_allocator PUBLIC pthread pmem)

option(BUILD_TESTING "Build the tests" ON)
if (BUILD_TESTING)
    set(TEST_SOURCE src/test.cpp)
    add_executable(allocator_test ${TEST_SOURCE})
    target_link_libraries(allocator_test PUBLIC pmem_allocator memkind)
endif ()