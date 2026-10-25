# This project is included after the native measurement library. Its manifest
# is separate, so adding retention records does not invalidate that library's
# already-compiled dependency-manifest fingerprint.
configure_file(
    "${CMAKE_BINARY_DIR}/source-sha256.txt"
    "${CMAKE_BINARY_DIR}/retention-source-sha256.txt"
    COPYONLY
)
set(retention_source_hashes "")
foreach(
    source
    src/benchmarks/libxrpl/ballast/retention/CMakeLists.txt
    src/benchmarks/libxrpl/ballast/retention/Retention.h
    src/benchmarks/libxrpl/ballast/retention/Retention.cpp
    src/benchmarks/libxrpl/ballast/retention/Main.cpp
    src/benchmarks/libxrpl/ballast/retention/Build.h.in
    src/benchmarks/libxrpl/ballast/retention/validate_csv.py
    src/tests/libxrpl/ballast/Retention.cpp
)
    file(SHA256 "${BALLAST_ROOT}/${source}" hash)
    string(APPEND retention_source_hashes "${hash}")
    file(
        APPEND "${CMAKE_BINARY_DIR}/retention-source-sha256.txt"
        "${hash}  harness/${source}\n"
    )
    set_property(
        DIRECTORY
        APPEND
        PROPERTY CMAKE_CONFIGURE_DEPENDS "${BALLAST_ROOT}/${source}"
    )
endforeach()
string(SHA256 retention_source_hash "${retention_source_hashes}")
file(
    SHA256 "${CMAKE_BINARY_DIR}/retention-source-sha256.txt"
    retention_dependency_hash
)
configure_file(Build.h.in "${CMAKE_CURRENT_BINARY_DIR}/RetentionBuild.h" @ONLY)
add_library(ballast.retention STATIC Retention.cpp)
target_include_directories(
    ballast.retention
    PRIVATE "${CMAKE_CURRENT_BINARY_DIR}" "${CMAKE_BINARY_DIR}/measurement"
)
target_link_libraries(ballast.retention PUBLIC ballast.measurement)
set_target_properties(
    ballast.retention
    PROPERTIES UNITY_BUILD OFF INTERPROCEDURAL_OPTIMIZATION FALSE
)
target_compile_options(ballast.retention PRIVATE -Wall -Wextra -Wpedantic)
add_executable(xrpl.ballast_retention Main.cpp)
target_link_libraries(
    xrpl.ballast_retention
    PRIVATE ballast.retention ballast.allocation_hooks
)
set_target_properties(
    xrpl.ballast_retention
    PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
        UNITY_BUILD OFF
        INTERPROCEDURAL_OPTIMIZATION FALSE
)
add_executable(
    xrpl.ballast_retention_tests
    ../../../../tests/libxrpl/ballast/Retention.cpp
)
target_link_libraries(
    xrpl.ballast_retention_tests
    PRIVATE ballast.retention ballast.allocation_hooks GTest::gtest_main
)
set_target_properties(
    xrpl.ballast_retention_tests
    PROPERTIES UNITY_BUILD OFF INTERPROCEDURAL_OPTIMIZATION FALSE
)
include(GoogleTest)
gtest_discover_tests(xrpl.ballast_retention_tests)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(
    NAME BallastRetention.csv_smoke
    COMMAND
        "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/validate_csv.py"
        --smoke "$<TARGET_FILE:xrpl.ballast_retention>"
)
