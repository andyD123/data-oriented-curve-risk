cmake_minimum_required(VERSION 3.16)
project(ladder CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Lane backend for the eight-wide kernel: STDX (std::experimental::simd), AVX512, AVX2, PORTABLE, or AUTO
set(LADDER_LANES "STDX" CACHE STRING "Lane backend")
set_property(CACHE LADDER_LANES PROPERTY STRINGS STDX AVX512 AVX2 PORTABLE AUTO)

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  message(WARNING "ladder is written for GCC 13+ / Clang with libstdc++ (std::experimental::simd, std::aligned_alloc). "
                  "On Windows use a WSL or MinGW toolchain.")
endif()

# ---- the library: header-only
add_library(ladder INTERFACE)
target_include_directories(ladder INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(ladder INTERFACE -march=native -mprefer-vector-width=512 -ffp-contract=fast -Wno-unused-result)
endif()
if(NOT LADDER_LANES STREQUAL "STDX")
  target_compile_definitions(ladder INTERFACE LADDER_LANES_${LADDER_LANES})
endif()

# ---- tests
enable_testing()
add_executable(test_ladder tests/test_ladder.cpp)
target_link_libraries(test_ladder ladder)
add_test(NAME ladder COMMAND test_ladder)

# ---- examples: each runs in its own output directory, with its data files copied next to it,
#      so a run configuration needs no working-directory setting
function(ladder_example name dir src)
  add_executable(${name} examples/${dir}/${src})
  target_link_libraries(${name} ladder)
  set_target_properties(${name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/examples/${dir})
  target_compile_definitions(${name} PRIVATE LADDER_EXAMPLE_DIR="${CMAKE_BINARY_DIR}/examples/${dir}")
  file(GLOB _data ${CMAKE_CURRENT_SOURCE_DIR}/examples/${dir}/*.txt ${CMAKE_CURRENT_SOURCE_DIR}/examples/${dir}/*.py)
  file(COPY ${_data} DESTINATION ${CMAKE_BINARY_DIR}/examples/${dir})
endfunction()

ladder_example(bench_paper     benchmark_paper bench_paper.cpp)      # §9 headline:    ./bench_paper 500000 1 3
ladder_example(scenario_bench  benchmark_paper scenario_bench.cpp)   # LRU date cache: ./scenario_bench 100000 1 2
ladder_example(adjoint_bench   benchmark_paper adjoint_bench.cpp)    # adjoints:       ./adjoint_bench 500000 2
ladder_example(bench_library   benchmark       bench.cpp)            # seasoned book:  ./bench_library 500000 0 3
ladder_example(aggregation     aggregation     agg.cpp)              # ladders vs maps
ladder_example(scan_wave       hagan_waves     scan_wave.cpp)        # uses recorded QuantLib output in its directory

# ---- QuantLib reference harnesses (optional): built only if QuantLib is found
find_library(QUANTLIB_LIB NAMES QuantLib)
find_path(QUANTLIB_INCLUDE ql/quantlib.hpp)
if(QUANTLIB_LIB AND QUANTLIB_INCLUDE)
  foreach(pair "ql_waves;hagan_waves;ql_waves.cpp" "ql_examples;quantlib_reconcile;ql_examples.cpp")
    list(GET pair 0 n)
    list(GET pair 1 d)
    list(GET pair 2 s)
    add_executable(${n} examples/${d}/${s})
    target_include_directories(${n} PRIVATE ${QUANTLIB_INCLUDE})
    target_link_libraries(${n} ${QUANTLIB_LIB})
    set_target_properties(${n} PROPERTIES CXX_STANDARD 17 RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/examples/${d})
    target_compile_definitions(${n} PRIVATE LADDER_EXAMPLE_DIR="${CMAKE_BINARY_DIR}/examples/${d}")
  endforeach()
  ladder_example(scan_reconcile quantlib_reconcile scan_reconcile.cpp)   # run ql_examples first (same directory)
  message(STATUS "QuantLib found: ql_waves, ql_examples, scan_reconcile targets enabled")
else()
  message(STATUS "QuantLib not found: QuantLib harnesses skipped (scan_wave still runs on recorded output)")
endif()
