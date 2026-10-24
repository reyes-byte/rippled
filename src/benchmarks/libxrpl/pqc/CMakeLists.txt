cmake_minimum_required(VERSION 3.24)
project(xrpl_pqc_bench LANGUAGES CXX)

# Deliberately standalone: no server, Conan installation, or protocol changes.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
find_package(OpenSSL 3.5 REQUIRED COMPONENTS Crypto)

add_library(pqc_support Layout.cpp Crypto.cpp)
target_include_directories(pqc_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../../..")
target_link_libraries(pqc_support PUBLIC OpenSSL::Crypto)
target_compile_options(pqc_support PRIVATE -Wall -Wextra -Wpedantic)
add_executable(pqc_bench Main.cpp)
target_link_libraries(pqc_bench PRIVATE pqc_support)
target_compile_options(pqc_bench PRIVATE -Wall -Wextra -Wpedantic)
execute_process(
    COMMAND git rev-parse HEAD
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    OUTPUT_VARIABLE PQC_SOURCE_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
target_compile_definitions(
    pqc_bench PRIVATE PQC_SOURCE_COMMIT="${PQC_SOURCE_COMMIT}"
)
# The base commit alone cannot identify an uncommitted hackathon prototype.
set(pqc_sources Layout.h Layout.cpp Crypto.h Crypto.cpp Main.cpp CMakeLists.txt)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${pqc_sources})
set(pqc_source_hashes "")
foreach(source IN LISTS pqc_sources)
    file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/${source}" source_hash)
    string(APPEND pqc_source_hashes "${source_hash}")
endforeach()
string(SHA256 pqc_implementation_hash "${pqc_source_hashes}")
target_compile_definitions(
    pqc_bench PRIVATE PQC_IMPLEMENTATION_SHA256="${pqc_implementation_hash}"
)

option(PQC_BUILD_TESTS "Build the standalone GoogleTest suite" ON)
if(PQC_BUILD_TESTS)
    include(CTest)
    find_package(GTest QUIET)
    if(NOT GTest_FOUND)
        include(FetchContent)
        # GoogleTest v1.17.0, matching the repository's test dependency.
        FetchContent_Declare(
            googletest
            GIT_REPOSITORY https://github.com/google/googletest.git
            GIT_TAG 52eb8108c5bdec04579160ae17225d66034bd723
        )
        set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
        set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(googletest)
    endif()
    add_executable(
        pqc_tests ../../../tests/libxrpl/pqc/BufferBench.cpp
    )
    target_link_libraries(pqc_tests PRIVATE pqc_support GTest::gtest_main)
    include(GoogleTest)
    gtest_discover_tests(pqc_tests)
endif()
