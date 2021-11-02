project(PMEM_ALLOCATOR
        DESCRIPTION "PMEM ALLOCATOR POC"
        LANGUAGES CXX C)

set(SOURCES src/pmem_allocator.cpp
        src/free_list.cpp)

add_library(pmem_allocator SHARED ${SOURCES})
target_link_libraries(pmem_allocator PUBLIC pthread pmem)