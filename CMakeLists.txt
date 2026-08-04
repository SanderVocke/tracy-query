cmake_minimum_required(VERSION 3.24)

project(tracy-query VERSION 0.1.0 LANGUAGES C CXX)

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

add_executable(tracy-query src/main.cpp)
target_link_libraries(tracy-query PRIVATE tracy_query_core)

if(MSVC)
    target_compile_options(tracy_query_core PRIVATE /W4 /permissive-)
    target_compile_options(tracy-query PRIVATE /W4 /permissive-)
else()
    target_compile_options(tracy_query_core PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(tracy-query PRIVATE -Wall -Wextra -Wpedantic)
endif()

include(GNUInstallDirs)
install(TARGETS tracy-query RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

include(CTest)
if(BUILD_TESTING)
    add_executable(tracy-query-unit-tests tests/unit.cpp)
    target_link_libraries(tracy-query-unit-tests PRIVATE tracy_query_core)
    add_test(NAME unit COMMAND tracy-query-unit-tests)

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
