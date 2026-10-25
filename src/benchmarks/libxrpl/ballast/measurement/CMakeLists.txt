# Included by the pinned native protocol project after its source manifest.
set(measurement_fixture_directory "${BALLAST_ROOT}/src/benchmarks/libxrpl/ballast/pq/native/fixtures/multisign")
set(measurement_fixture_records "")
foreach(slug secp256k1-1 secp256k1-8 secp256k1-32 ml-dsa-44-1 ml-dsa-44-8 ml-dsa-44-32)
    file(SHA256 "${measurement_fixture_directory}/${slug}.json" metadata_hash)
    file(SHA256 "${measurement_fixture_directory}/${slug}.hex" hex_hash)
    string(APPEND measurement_fixture_records "    {\"${slug}\", \"${metadata_hash}\", \"${hex_hash}\"},\n")
endforeach()

set(measurement_source_hashes "")
foreach(source
    src/benchmarks/libxrpl/ballast/measurement/CMakeLists.txt
    src/benchmarks/libxrpl/ballast/measurement/Measurement.h
    src/benchmarks/libxrpl/ballast/measurement/Measurement.cpp
    src/benchmarks/libxrpl/ballast/measurement/Main.cpp
    src/benchmarks/libxrpl/ballast/measurement/Build.h.in
    src/benchmarks/libxrpl/ballast/measurement/validate_csv.py
    src/benchmarks/libxrpl/ballast/accounting/AllocationTracker.h
    src/benchmarks/libxrpl/ballast/accounting/AllocationTracker.cpp
    src/tests/libxrpl/ballast/Measurement.cpp
)
    file(SHA256 "${BALLAST_ROOT}/${source}" hash)
    string(APPEND measurement_source_hashes "${hash}")
    file(APPEND "${CMAKE_BINARY_DIR}/source-sha256.txt" "${hash}  harness/${source}\n")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${BALLAST_ROOT}/${source}")
endforeach()
string(SHA256 measurement_source_hash "${measurement_source_hashes}")
file(SHA256 "${CMAKE_BINARY_DIR}/source-sha256.txt" measurement_dependency_hash)
configure_file(Build.h.in "${CMAKE_CURRENT_BINARY_DIR}/MeasurementBuild.h" @ONLY)

add_library(ballast.measurement STATIC Measurement.cpp)
target_include_directories(ballast.measurement PUBLIC "${BALLAST_ROOT}/src" PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(ballast.measurement PUBLIC ballast.native.protocol ballast.native.crypto ballast.native.git ballast.native.json ballast.native.basics ballast.native.beast)
set_target_properties(ballast.measurement PROPERTIES UNITY_BUILD OFF INTERPROCEDURAL_OPTIMIZATION FALSE)
target_compile_options(ballast.measurement PRIVATE -Wall -Wextra -Wpedantic)

add_executable(xrpl.ballast_measure Main.cpp)
target_link_libraries(xrpl.ballast_measure PRIVATE ballast.measurement ballast.allocation_hooks)
set_target_properties(xrpl.ballast_measure PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}" UNITY_BUILD OFF INTERPROCEDURAL_OPTIMIZATION FALSE)
add_executable(xrpl.ballast_measure_tests ../../../../tests/libxrpl/ballast/Measurement.cpp)
target_link_libraries(xrpl.ballast_measure_tests PRIVATE ballast.measurement ballast.allocation_hooks GTest::gtest_main)
set_target_properties(xrpl.ballast_measure_tests PROPERTIES UNITY_BUILD OFF INTERPROCEDURAL_OPTIMIZATION FALSE)
include(GoogleTest)
gtest_discover_tests(xrpl.ballast_measure_tests NO_PRETTY_VALUES)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME BallastMeasurement.csv_smoke COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/validate_csv.py" --smoke "$<TARGET_FILE:xrpl.ballast_measure>")
