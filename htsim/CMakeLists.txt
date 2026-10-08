cmake_minimum_required(VERSION 3.16)
project(suet_htsim LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# The simulator requires assertions even in release builds.
set(CMAKE_CXX_FLAGS_RELEASE "-O3")
# Keep the integration's compiler options consistent with the simulator.
add_compile_options(-Wall -g -O3)

set(HTSIM_SOURCE_DIR "" CACHE PATH "uet-htsim repository checkout")
if(NOT EXISTS "${HTSIM_SOURCE_DIR}/htsim/sim/CMakeLists.txt")
    message(FATAL_ERROR "Set HTSIM_SOURCE_DIR to a uet-htsim repository checkout")
endif()
add_subdirectory("${HTSIM_SOURCE_DIR}/htsim/sim" "network" EXCLUDE_FROM_ALL)

# Uses only libfabric's installed public provider API. No libfabric source patch
# is needed for this backend; the SUET build must contain the clock extension.
set(LIBFABRIC_ROOT "" CACHE PATH "Prefix containing the SUET-enabled libfabric build")
set(SUET_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../suet" CACHE PATH "SUET provider source directory")
find_path(LIBFABRIC_INCLUDE_DIR rdma/fabric.h HINTS "${LIBFABRIC_ROOT}/include" REQUIRED)
find_library(LIBFABRIC_LIBRARY fabric HINTS "${LIBFABRIC_ROOT}/lib" REQUIRED)
if(NOT EXISTS "${SUET_SRC_DIR}/src/suet_ext.h")
    message(FATAL_ERROR "Set SUET_SRC_DIR to the provider source directory containing src/suet_ext.h")
endif()
add_library(fabric_dgram SHARED dgram.cpp)
set_target_properties(fabric_dgram PROPERTIES OUTPUT_NAME "htsim-fi" SUFFIX ".so")
target_include_directories(fabric_dgram PUBLIC "${LIBFABRIC_INCLUDE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(fabric_dgram PUBLIC "${LIBFABRIC_LIBRARY}")
add_executable(htsim_suet suet_main.cpp)
target_include_directories(htsim_suet PRIVATE "${SUET_SRC_DIR}/src")
target_link_libraries(htsim_suet PRIVATE fabric_dgram htsim_dc htsim)

option(ENABLE_SUET_TESTS "Enable standalone provider and integration tests" ON)
if(ENABLE_SUET_TESTS)
    enable_testing()
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_executable(fabric_dgram_test dgram_test.cpp)
    target_link_libraries(fabric_dgram_test PRIVATE fabric_dgram)
    add_test(NAME fabric_dgram_contract COMMAND fabric_dgram_test)
    add_test(NAME suet_simulation COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/simulation_test.py" "$<TARGET_FILE:htsim_suet>"
        "${HTSIM_SOURCE_DIR}/htsim/sim/datacenter/topologies/leaf_spine_tiny.topo")
    set_tests_properties(suet_simulation PROPERTIES TIMEOUT 300)
    set_tests_properties(fabric_dgram_contract PROPERTIES
        ENVIRONMENT "FI_PROVIDER_PATH=$<TARGET_FILE_DIR:fabric_dgram>")
endif()

option(SUET_SANITIZE "Instrument the provider and runner with ASan/UBSan" OFF)
if(SUET_SANITIZE)
    foreach(target fabric_dgram htsim_suet)
        target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endforeach()
    if(ENABLE_SUET_TESTS)
        target_compile_options(fabric_dgram_test PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(fabric_dgram_test PRIVATE -fsanitize=address,undefined)
    endif()
endif()
