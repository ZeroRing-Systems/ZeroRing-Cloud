cmake_minimum_required(VERSION 3.28.3)
project(ZeroRingCloud LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(USE_POSTGRES "Link against libpqxx for PostgreSQL-backed VFS" OFF)

add_executable(server
    backend/server.cpp
    backend/db_manager_pg.cpp
    backend/db_manager_mem.cpp
)

target_include_directories(server PRIVATE backend)

find_package(Threads REQUIRED)
target_link_libraries(server Threads::Threads)

find_package(OpenSSL REQUIRED)
target_link_libraries(server OpenSSL::SSL OpenSSL::Crypto)

if(USE_POSTGRES)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(PQXX QUIET libpqxx)
    endif()

    if(PQXX_FOUND)
        target_include_directories(server PRIVATE ${PQXX_INCLUDE_DIRS})
        target_link_libraries(server ${PQXX_LIBRARIES})
    else()
        find_library(PQXX_LIB pqxx)
        find_library(PQ_LIB pq)
        if(PQXX_LIB AND PQ_LIB)
            target_link_libraries(server ${PQXX_LIB} ${PQ_LIB})
        else()
            message(FATAL_ERROR
                "USE_POSTGRES=ON but libpqxx/libpq not found.\n"
                "Install: sudo apt install libpqxx-dev libpq-dev\n"
                "Or run without: cmake -DUSE_POSTGRES=OFF ..")
        endif()
    endif()

    target_compile_definitions(server PRIVATE USE_POSTGRES)
    message(STATUS "PostgreSQL backend: ENABLED")
else()
    message(STATUS "PostgreSQL backend: DISABLED (using in-memory VFS)")
endif()