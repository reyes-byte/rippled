cmake_minimum_required(VERSION 3.24)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(ballast_allocation_calibration LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 23)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    enable_testing()
endif()
get_filename_component(BALLAST_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../.." ABSOLUTE)
find_package(GTest REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

# Link the object directly into selected executables. Never add it to libxrpl
# or the existing ownership microbenchmark. Keep probes/hooks in separate TUs.
add_library(ballast.allocation_hooks OBJECT AllocationTracker.cpp)
target_include_directories(ballast.allocation_hooks PUBLIC "${BALLAST_ROOT}/src")
add_library(ballast.allocation_probes OBJECT Probes.cpp)
target_include_directories(ballast.allocation_probes PUBLIC "${BALLAST_ROOT}/src")
target_link_libraries(ballast.allocation_probes PRIVATE OpenSSL::Crypto)
set_target_properties(ballast.allocation_hooks ballast.allocation_probes PROPERTIES UNITY_BUILD OFF INTERPROCEDURAL_OPTIMIZATION FALSE)
target_compile_options(ballast.allocation_hooks PRIVATE -Wall -Wextra -Wpedantic)

add_executable(xrpl.ballast_allocation_calibration ../../../../tests/libxrpl/ballast/AllocationAccounting.cpp)
target_link_libraries(xrpl.ballast_allocation_calibration PRIVATE ballast.allocation_hooks ballast.allocation_probes OpenSSL::Crypto Threads::Threads GTest::gtest_main)
set_target_properties(xrpl.ballast_allocation_calibration PROPERTIES UNITY_BUILD OFF INTERPROCEDURAL_OPTIMIZATION FALSE)
include(GoogleTest)
gtest_discover_tests(xrpl.ballast_allocation_calibration NO_PRETTY_VALUES)

file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/accounting-source-sha256.txt" "")
foreach(source AllocationTracker.h AllocationTracker.cpp Probes.h Probes.cpp CMakeLists.txt ../../../../tests/libxrpl/ballast/AllocationAccounting.cpp)
    file(SHA256 "${CMAKE_CURRENT_LIST_DIR}/${source}" hash)
    file(APPEND "${CMAKE_CURRENT_BINARY_DIR}/accounting-source-sha256.txt" "${hash}  ${source}\n")
endforeach()
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/accounting-build-config.txt" CONTENT
"scope=cpp_allocation_calibration
configuration=$<CONFIG>
cxx_compiler=${CMAKE_CXX_COMPILER}
cxx_compiler_version=${CMAKE_CXX_COMPILER_VERSION}
cxx_standard=23
cxx_flags=${CMAKE_CXX_FLAGS}
cxx_release_flags=${CMAKE_CXX_FLAGS_RELEASE}
exe_linker_flags=${CMAKE_EXE_LINKER_FLAGS}
hooks_and_probes_unity=OFF
hooks_and_probes_ipo=OFF
coverage=global_cpp_new_delete_requested_bytes
attribution=allocating_thread_innermost_scope
frees=allocation_origin_any_thread
tracker_metadata=excluded
rss=not_measured
")
