cmake_minimum_required(VERSION 3.24)

project(tracy-query VERSION 0.1.0 LANGUAGES C CXX)

set(_tracy_query_static_default OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(_tracy_query_static_default ON)
endif()
option(
    TRACY_QUERY_FULLY_STATIC
    "Build the Linux tracy-query executable without dynamic dependencies"
    ${_tracy_query_static_default}
)
unset(_tracy_query_static_default)

if(TRACY_QUERY_FULLY_STATIC AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "TRACY_QUERY_FULLY_STATIC is currently supported only on Linux")
endif()

# Keep Windows release binaries independent of the separately installed MSVC
# and Universal C runtimes. Windows system DLLs remain normal OS dependencies.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# Tracy and every bundled third-party dependency are always linked into the
# executable. No installed Tracy package, library, or profiler binary is used.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build dependencies as static libraries" FORCE)

include(cmake/TracyServer.cmake)

add_library(
    tracy_query_core STATIC
    src/adapter.cpp
    src/cli.cpp
    src/commands.cpp
    src/model.cpp
    src/output.cpp
    src/query.cpp
    src/time.cpp
    src/trace.cpp
)
target_compile_features(tracy_query_core PUBLIC cxx_std_20)
target_include_directories(tracy_query_core PUBLIC include)
target_link_libraries(tracy_query_core PUBLIC TracyQuery::TracyServer)

function(tracy_query_apply_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

function(tracy_query_configure_executable target)
    tracy_query_apply_warnings(${target})
    if(TRACY_QUERY_FULLY_STATIC)
        target_link_options(${target} PRIVATE -static)
        add_custom_command(
            TARGET ${target}
            POST_BUILD
            COMMAND
                ${CMAKE_COMMAND}
                -DTRACY_QUERY_EXECUTABLE=$<TARGET_FILE:${target}>
                -DTRACY_QUERY_READELF=${CMAKE_READELF}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyStaticExecutable.cmake
            COMMENT "Verifying that ${target} has no dynamic dependencies"
            VERBATIM
        )
    endif()
endfunction()

add_executable(tracy-query src/main.cpp)
target_link_libraries(tracy-query PRIVATE tracy_query_core)
tracy_query_apply_warnings(tracy_query_core)
tracy_query_configure_executable(tracy-query)

add_library(tracy_collector_core STATIC src/collector/collector.cpp)
target_compile_features(tracy_collector_core PUBLIC cxx_std_20)
target_include_directories(tracy_collector_core PUBLIC include)
target_link_libraries(tracy_collector_core PRIVATE TracyQuery::TracyServer)
tracy_query_apply_warnings(tracy_collector_core)

add_executable(tracy-collector src/collector/main.cpp)
target_link_libraries(tracy-collector PRIVATE tracy_collector_core TracyQuery::TracyServer)
tracy_query_configure_executable(tracy-collector)

include(GNUInstallDirs)
install(TARGETS tracy-query tracy-collector RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

include(CTest)
if(BUILD_TESTING)
    add_executable(tracy-query-unit-tests tests/unit.cpp)
    target_link_libraries(tracy-query-unit-tests PRIVATE tracy_query_core)
    add_test(NAME unit COMMAND tracy-query-unit-tests)

    add_executable(tracy-collector-unit-tests tests/collector_unit.cpp)
    target_link_libraries(tracy-collector-unit-tests PRIVATE tracy_collector_core)
    tracy_query_apply_warnings(tracy-collector-unit-tests)
    add_test(NAME collector-unit COMMAND tracy-collector-unit-tests)

    add_library(
        tracy_collector_tracy_client STATIC
        "${tracy_0131_SOURCE_DIR}/public/TracyClient.cpp"
    )
    target_compile_features(tracy_collector_tracy_client PUBLIC cxx_std_20)
    target_compile_definitions(
        tracy_collector_tracy_client
        PUBLIC TRACY_ENABLE TRACY_ON_DEMAND TRACY_ONLY_LOCALHOST
    )
    target_include_directories(
        tracy_collector_tracy_client
        SYSTEM PUBLIC "${tracy_0131_SOURCE_DIR}/public"
    )
    target_link_libraries(
        tracy_collector_tracy_client
        PUBLIC Threads::Threads ${CMAKE_DL_LIBS}
    )
    # Tracy's bundled libbacktrace reads DWARF tag values through an enum while
    # decoding sanitizer-instrumented binaries. Keep all other UBSan checks.
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(tracy_collector_tracy_client PRIVATE -fno-sanitize=enum)
    endif()

    add_executable(tracy-collector-live-fixture tests/fixtures/tracy_core_fixture.cpp)
    target_link_libraries(tracy-collector-live-fixture PRIVATE tracy_collector_tracy_client)

    add_executable(tracy-collector-crash-fixture tests/fixtures/tracy_crash_fixture.cpp)
    target_link_libraries(tracy-collector-crash-fixture PRIVATE tracy_collector_tracy_client)

    add_executable(tracy-collector-long-fixture tests/fixtures/tracy_long_fixture.cpp)
    target_link_libraries(tracy-collector-long-fixture PRIVATE tracy_collector_tracy_client)

    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(
        NAME collector-protocol-live
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/collector_integration.py
                --collector $<TARGET_FILE:tracy-collector>
                --query $<TARGET_FILE:tracy-query>
                --live $<TARGET_FILE:tracy-collector-live-fixture>
                --crash $<TARGET_FILE:tracy-collector-crash-fixture>
                --long $<TARGET_FILE:tracy-collector-long-fixture>
                --work ${CMAKE_CURRENT_BINARY_DIR}/collector-integration
    )
    set_tests_properties(collector-protocol-live PROPERTIES TIMEOUT 90)

    find_program(CARGO_NEXTEST_EXECUTABLE NAMES cargo-nextest)
    if(CARGO_NEXTEST_EXECUTABLE)
        add_test(
            NAME collector-nextest-contract
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/nextest_orchestrator.py
                    --collector $<TARGET_FILE:tracy-collector>
                    --query $<TARGET_FILE:tracy-query>
                    --nextest ${CARGO_NEXTEST_EXECUTABLE}
                    --fixture ${CMAKE_CURRENT_SOURCE_DIR}/tests/nextest-fixture
                    --work ${CMAKE_CURRENT_BINARY_DIR}/nextest-integration
        )
        set_tests_properties(collector-nextest-contract PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)
    else()
        message(STATUS "cargo-nextest not found; the dedicated CI job runs the nextest contract")
    endif()

    add_executable(
        tracy-query-structural-fixture-generator
        tests/fixtures/tracy_structural_fixture.cpp
    )
    target_link_libraries(
        tracy-query-structural-fixture-generator
        PRIVATE TracyQuery::TracyServer
    )
    add_test(
        NAME fixture-checksums
        COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/verify_checksums.cmake
    )

    add_test(NAME cli-help COMMAND tracy-query --help)
    add_test(NAME cli-query-help COMMAND tracy-query query --help)

    add_test(NAME cli-requires-trace-file COMMAND tracy-query)
    set_tests_properties(cli-requires-trace-file PROPERTIES WILL_FAIL TRUE)

    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/not-a-trace.tracy" "not a Tracy trace\n")
    add_test(
        NAME cli-rejects-invalid-trace
        COMMAND tracy-query "${CMAKE_CURRENT_BINARY_DIR}/not-a-trace.tracy"
    )
    set_tests_properties(cli-rejects-invalid-trace PROPERTIES WILL_FAIL TRUE)

    set(REFERENCE_TRACE "${CMAKE_CURRENT_SOURCE_DIR}/traces/monkey-playground/0001-application.tracy")
    if(EXISTS "${REFERENCE_TRACE}")
        add_test(NAME cli-check-reference COMMAND tracy-query check "${REFERENCE_TRACE}")
        add_test(NAME cli-check-multiple COMMAND tracy-query check "${REFERENCE_TRACE}" "${REFERENCE_TRACE}")
        add_test(NAME cli-range-reference COMMAND tracy-query range "${REFERENCE_TRACE}")
        set_tests_properties(cli-range-reference PROPERTIES
            PASS_REGULAR_EXPRESSION "\"duration_ns\":16310852272"
        )
        add_test(NAME cli-info-reference COMMAND tracy-query info "${REFERENCE_TRACE}")
        set_tests_properties(cli-info-reference PROPERTIES
            PASS_REGULAR_EXPRESSION "\"cpu_zone_count\":617218"
        )
        add_test(
            NAME cli-sources-reference
            COMMAND tracy-query sources --kind plot --plot callback "${REFERENCE_TRACE}"
        )
        set_tests_properties(cli-sources-reference PROPERTIES
            PASS_REGULAR_EXPRESSION "BackendWrapper/callback_last_ns"
        )
        add_test(
            NAME cli-query-cpu-zone-count
            COMMAND tracy-query query --kind cpu-zone
                    --filter "cpu-zone.name=^engine\\.rt\\.cycle$" --count "${REFERENCE_TRACE}"
        )
        set_tests_properties(cli-query-cpu-zone-count PROPERTIES
            PASS_REGULAR_EXPRESSION "\"count\":2369"
        )
        add_test(
            NAME cli-query-plot-point
            COMMAND tracy-query query --kind plot --plot "^BackendWrapper/cycles$"
                    --at 15s --latest --next "${REFERENCE_TRACE}"
        )
        set_tests_properties(cli-query-plot-point PROPERTIES
            PASS_REGULAR_EXPRESSION "\"value\":2275.*\n.*\"value\":2277"
        )
        add_test(
            NAME cli-query-mixed-range
            COMMAND tracy-query query --kind message,plot --from 1s --to 1.01s
                    --limit 3 "${REFERENCE_TRACE}"
        )
        set_tests_properties(cli-query-mixed-range PROPERTIES
            PASS_REGULAR_EXPRESSION "\"timestamp_ns\":1001809406"
        )
        add_test(
            NAME cli-query-active-zone
            COMMAND tracy-query query --kind cpu-zone --at 5s --active --limit 1
                    "${REFERENCE_TRACE}"
        )
        set_tests_properties(cli-query-active-zone PROPERTIES
            PASS_REGULAR_EXPRESSION "\"name\":\"app.lifecycle\""
        )
        add_test(
            NAME cli-query-all-count
            COMMAND tracy-query query --kind all --count --group-by kind "${REFERENCE_TRACE}"
        )
        set_tests_properties(cli-query-all-count PROPERTIES
            PASS_REGULAR_EXPRESSION "\"kind\":\"hardware-sample\""
        )
        add_test(
            NAME cli-query-count-equivalence
            COMMAND ${CMAKE_COMMAND}
                    -DTRACY_QUERY=$<TARGET_FILE:tracy-query>
                    -DTRACE_FILE=${REFERENCE_TRACE}
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/compare_count.cmake
        )
    endif()

    set(SYNTHETIC_TRACE "${CMAKE_CURRENT_SOURCE_DIR}/traces/synthetic/0001-core.tracy")
    if(EXISTS "${SYNTHETIC_TRACE}")
        add_test(NAME cli-check-synthetic COMMAND tracy-query check "${SYNTHETIC_TRACE}")
        add_test(
            NAME cli-query-synthetic-zones
            COMMAND tracy-query query --kind cpu-zone --filter "cpu-zone.name=^fixture\\."
                    --count "${SYNTHETIC_TRACE}"
        )
        set_tests_properties(cli-query-synthetic-zones PROPERTIES PASS_REGULAR_EXPRESSION "\"count\":2")
        add_test(
            NAME cli-query-synthetic-zone-parent
            COMMAND tracy-query query --kind cpu-zone --zone-parent "^fixture\\.root$"
                    --count "${SYNTHETIC_TRACE}"
        )
        set_tests_properties(cli-query-synthetic-zone-parent PROPERTIES PASS_REGULAR_EXPRESSION "\"count\":1")
        add_test(
            NAME cli-query-synthetic-messages
            COMMAND tracy-query query --kind message --filter "message.text=^fixture"
                    --count "${SYNTHETIC_TRACE}"
        )
        set_tests_properties(cli-query-synthetic-messages PROPERTIES PASS_REGULAR_EXPRESSION "\"count\":2")
        add_test(
            NAME cli-query-synthetic-plot
            COMMAND tracy-query query --kind plot --plot "^fixture\\.plot$"
                    --count "${SYNTHETIC_TRACE}"
        )
        set_tests_properties(cli-query-synthetic-plot PROPERTIES PASS_REGULAR_EXPRESSION "\"count\":2")
        add_test(
            NAME cli-query-synthetic-frame
            COMMAND tracy-query query --kind frame --frame-set "^fixture\\.frame$"
                    --count "${SYNTHETIC_TRACE}"
        )
        set_tests_properties(cli-query-synthetic-frame PROPERTIES PASS_REGULAR_EXPRESSION "\"count\":1")
        add_test(
            NAME cli-query-synthetic-frame-image
            COMMAND tracy-query query --kind frame-image --count "${SYNTHETIC_TRACE}"
        )
        set_tests_properties(cli-query-synthetic-frame-image PROPERTIES PASS_REGULAR_EXPRESSION "\"count\":1")
        add_test(
            NAME cli-query-synthetic-lock
            COMMAND tracy-query query --kind lock --count "${SYNTHETIC_TRACE}"
        )
        set_tests_properties(cli-query-synthetic-lock PROPERTIES PASS_REGULAR_EXPRESSION "\"count\":3")
        add_test(
            NAME cli-query-synthetic-memory
            COMMAND tracy-query query --kind memory --memory-pool "^fixture\\.pool$"
                    --count "${SYNTHETIC_TRACE}"
        )
        set_tests_properties(cli-query-synthetic-memory PROPERTIES PASS_REGULAR_EXPRESSION "\"count\":2")
        add_test(
            NAME cli-query-synthetic-inclusive-boundary
            COMMAND tracy-query query --kind message --filter "message.text=^fixture"
                    --from 240980445ns --to 240980445ns --count "${SYNTHETIC_TRACE}"
        )
        set_tests_properties(cli-query-synthetic-inclusive-boundary PROPERTIES PASS_REGULAR_EXPRESSION "\"count\":1")
        add_test(
            NAME cli-query-synthetic-start-match
            COMMAND tracy-query query --kind cpu-zone --range-match start
                    --from 240982639ns --to 240982639ns --count "${SYNTHETIC_TRACE}"
        )
        set_tests_properties(cli-query-synthetic-start-match PROPERTIES PASS_REGULAR_EXPRESSION "\"count\":1")
        add_test(
            NAME cli-query-synthetic-contained-match
            COMMAND tracy-query query --kind cpu-zone --range-match contained
                    --from 240982639ns --to 243064961ns --count "${SYNTHETIC_TRACE}"
        )
        set_tests_properties(cli-query-synthetic-contained-match PROPERTIES PASS_REGULAR_EXPRESSION "\"count\":1")
        add_test(
            NAME cli-query-synthetic-active-nesting
            COMMAND tracy-query query --kind cpu-zone --at 240982639ns --active
                    --count "${SYNTHETIC_TRACE}"
        )
        set_tests_properties(cli-query-synthetic-active-nesting PROPERTIES PASS_REGULAR_EXPRESSION "\"count\":2")
        add_test(
            NAME cli-query-synthetic-next-outside
            COMMAND tracy-query query --kind message --filter "message.text=^fixture"
                    --at 0ns --next --count "${SYNTHETIC_TRACE}"
        )
        set_tests_properties(cli-query-synthetic-next-outside PROPERTIES PASS_REGULAR_EXPRESSION "\"count\":1")
        add_test(
            NAME cli-query-synthetic-point-deduplication
            COMMAND tracy-query query --kind message --filter "message.text=^fixture"
                    --at 240980445ns --latest --next --count "${SYNTHETIC_TRACE}"
        )
        set_tests_properties(cli-query-synthetic-point-deduplication PROPERTIES PASS_REGULAR_EXPRESSION "\"count\":1")
        add_test(
            NAME cli-query-synthetic-empty-kind
            COMMAND tracy-query query --kind gpu-zone --count "${SYNTHETIC_TRACE}"
        )
        set_tests_properties(cli-query-synthetic-empty-kind PROPERTIES PASS_REGULAR_EXPRESSION "\"count\":0")
        add_test(
            NAME cli-query-synthetic-mixed-count
            COMMAND tracy-query query --kind all --count "${SYNTHETIC_TRACE}"
        )
        set_tests_properties(cli-query-synthetic-mixed-count PROPERTIES PASS_REGULAR_EXPRESSION "\"count\":22")
        add_test(
            NAME cli-query-synthetic-scoped-regex
            COMMAND tracy-query query --kind message,plot
                    --filter "message.text=^fixture" --filter "plot.name=^fixture\\.plot$"
                    --count "${SYNTHETIC_TRACE}"
        )
        set_tests_properties(cli-query-synthetic-scoped-regex PROPERTIES PASS_REGULAR_EXPRESSION "\"count\":4")
        add_test(
            NAME cli-query-synthetic-file-regex
            COMMAND tracy-query query --kind cpu-zone
                    --filter "cpu-zone.file=tracy_core_fixture\\.cpp$"
                    --count "${SYNTHETIC_TRACE}"
        )
        set_tests_properties(cli-query-synthetic-file-regex PROPERTIES PASS_REGULAR_EXPRESSION "\"count\":2")
        add_test(
            NAME cli-query-synthetic-thread-scope
            COMMAND tracy-query query --kind message --thread "^Main thread$"
                    --count "${SYNTHETIC_TRACE}"
        )
        set_tests_properties(cli-query-synthetic-thread-scope PROPERTIES PASS_REGULAR_EXPRESSION "\"count\":2")
    endif()

    if(EXISTS "${REFERENCE_TRACE}" AND EXISTS "${SYNTHETIC_TRACE}")
        add_test(
            NAME cli-query-multiple-traces
            COMMAND ${CMAKE_COMMAND}
                    -DTRACY_QUERY=$<TARGET_FILE:tracy-query>
                    -DREFERENCE_TRACE=${REFERENCE_TRACE}
                    -DSYNTHETIC_TRACE=${SYNTHETIC_TRACE}
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/verify_multi_trace.cmake
        )
    endif()

    set(CRASH_TRACE "${CMAKE_CURRENT_SOURCE_DIR}/traces/synthetic/0002-crash.tracy")
    if(EXISTS "${CRASH_TRACE}")
        add_test(
            NAME cli-query-synthetic-crash
            COMMAND tracy-query query --kind crash "${CRASH_TRACE}"
        )
        set_tests_properties(cli-query-synthetic-crash PROPERTIES
            PASS_REGULAR_EXPRESSION "Abort signal from abort\\(\\)"
        )
    endif()

    set(SAMPLING_TRACE "${CMAKE_CURRENT_SOURCE_DIR}/traces/synthetic/0003-sampling.tracy")
    if(EXISTS "${SAMPLING_TRACE}")
        add_test(NAME cli-query-synthetic-samples
                 COMMAND tracy-query query --kind sample --count "${SAMPLING_TRACE}")
        set_tests_properties(cli-query-synthetic-samples PROPERTIES PASS_REGULAR_EXPRESSION "\"count\":1757")
        add_test(NAME cli-query-synthetic-ghost-zones
                 COMMAND tracy-query query --kind ghost-zone --count "${SAMPLING_TRACE}")
        set_tests_properties(cli-query-synthetic-ghost-zones PROPERTIES PASS_REGULAR_EXPRESSION "\"count\":1705")
        add_test(NAME cli-query-synthetic-hardware
                 COMMAND tracy-query query --kind hardware-sample --count "${SAMPLING_TRACE}")
        set_tests_properties(cli-query-synthetic-hardware PROPERTIES PASS_REGULAR_EXPRESSION "\"count\":8059")
        add_test(
            NAME cli-query-synthetic-symbol-regex
            COMMAND tracy-query query --kind hardware-sample --detail full
                    --filter "hardware-sample.symbol=SysTraceWorker"
                    --count "${SAMPLING_TRACE}"
        )
        set_tests_properties(cli-query-synthetic-symbol-regex PROPERTIES PASS_REGULAR_EXPRESSION "\"count\":83")
    endif()

    set(STRUCTURAL_TRACE "${CMAKE_CURRENT_SOURCE_DIR}/traces/synthetic/0004-structural.tracy")
    if(EXISTS "${STRUCTURAL_TRACE}")
        add_test(
            NAME cli-query-synthetic-structural
            COMMAND tracy-query query --kind gpu-zone,context-switch,cpu-slice,cpu-usage
                    --count --group-by kind "${STRUCTURAL_TRACE}"
        )
        set_tests_properties(cli-query-synthetic-structural PROPERTIES
            PASS_REGULAR_EXPRESSION "\"kind\":\"gpu-zone\",\"count\":1"
        )
    endif()
    add_test(
        NAME structural-fixture-roundtrip
        COMMAND ${CMAKE_COMMAND}
                -DGENERATOR=$<TARGET_FILE:tracy-query-structural-fixture-generator>
                -DTRACY_QUERY=$<TARGET_FILE:tracy-query>
                -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/generated-structural.tracy
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/verify_structural_generator.cmake
    )
endif()
