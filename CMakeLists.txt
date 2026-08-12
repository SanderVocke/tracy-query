cmake_minimum_required(VERSION 3.24)

project(tracy-query VERSION 0.2.0 LANGUAGES C CXX)

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

# Keep release executables independent of separately installed MSVC/UCRT
# redistributables. Cargo's build script disables this override so the native
# archive follows Rust's selected crt-static target feature instead.
option(TRACY_QUERY_STATIC_MSVC_RUNTIME "Use the static MSVC runtime" ON)
if(MSVC AND TRACY_QUERY_STATIC_MSVC_RUNTIME)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# Tracy and every bundled third-party dependency are always linked into the
# executable. No installed Tracy package, library, or profiler binary is used.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build dependencies as static libraries" FORCE)

include(cmake/TracyServer.cmake)

# These cache switches mirror tracy-client-sys features when this project is
# configured by its build script. Repository-native fixtures intentionally use
# the low-overhead defaults.
option(TRACY_NATIVE_ON_DEMAND "Enable Tracy on-demand mode in native client libraries" ON)
option(TRACY_NATIVE_MANUAL_LIFETIME "Enable Tracy manual lifetime in ordinary client library" OFF)
option(TRACY_NATIVE_DELAYED_INIT "Enable Tracy delayed initialization in ordinary client library" OFF)
option(TRACY_NATIVE_FLUSH_ON_EXIT "Keep Tracy alive to flush on process exit" OFF)
option(TRACY_NATIVE_DEBUGINFOD "Enable Tracy debuginfod integration" OFF)
option(TRACY_NATIVE_ONLY_LOCALHOST "Restrict ordinary Tracy client sockets to localhost" OFF)
option(TRACY_NATIVE_ONLY_IPV4 "Restrict ordinary Tracy client sockets to IPv4" OFF)
option(TRACY_NATIVE_TIMER_FALLBACK "Use Tracy's timer fallback" OFF)
option(TRACY_NATIVE_SYSTEM_TRACING "Enable Tracy system tracing" OFF)
option(TRACY_NATIVE_CONTEXT_SWITCH "Enable Tracy context switch tracing" OFF)
option(TRACY_NATIVE_SAMPLING "Enable Tracy sampling" OFF)
option(TRACY_NATIVE_CODE_TRANSFER "Enable Tracy code transfer" OFF)
option(TRACY_NATIVE_BROADCAST "Enable Tracy network discovery broadcast" OFF)
option(TRACY_NATIVE_CALLSTACK_INLINES "Enable Tracy callstack inlines" OFF)
option(TRACY_NATIVE_CRASH_HANDLER "Enable Tracy crash handler" OFF)
option(TRACY_NATIVE_VERIFY "Enable Tracy instrumentation verification" OFF)
option(TRACY_NATIVE_DEMANGLE "Enable Tracy demangling hook" OFF)
option(TRACY_NATIVE_FIBERS "Enable Tracy fibers" OFF)

function(tracy_native_configure_client target force_manual_lifetime)
    target_compile_definitions(${target} PUBLIC TRACY_ENABLE)
    if(force_manual_lifetime OR TRACY_NATIVE_MANUAL_LIFETIME)
        target_compile_definitions(${target} PUBLIC TRACY_DELAYED_INIT TRACY_MANUAL_LIFETIME)
    elseif(TRACY_NATIVE_DELAYED_INIT)
        target_compile_definitions(${target} PUBLIC TRACY_DELAYED_INIT)
    endif()
    if(TRACY_NATIVE_FLUSH_ON_EXIT)
        target_compile_definitions(${target} PUBLIC TRACY_NO_EXIT)
    endif()
    if(TRACY_NATIVE_DEBUGINFOD)
        target_compile_definitions(${target} PUBLIC TRACY_DEBUGINFOD)
        target_link_libraries(${target} PUBLIC debuginfod)
    endif()
    foreach(_feature IN ITEMS ON_DEMAND ONLY_LOCALHOST ONLY_IPV4 TIMER_FALLBACK DEMANGLE FIBERS)
        if(TRACY_NATIVE_${_feature})
            target_compile_definitions(${target} PUBLIC TRACY_${_feature})
        endif()
    endforeach()
    foreach(_feature IN ITEMS SYSTEM_TRACING CONTEXT_SWITCH SAMPLING CODE_TRANSFER BROADCAST CALLSTACK_INLINES CRASH_HANDLER VERIFY)
        if(NOT TRACY_NATIVE_${_feature})
            target_compile_definitions(${target} PUBLIC TRACY_NO_${_feature})
        endif()
    endforeach()
endfunction()

add_library(tracy_instrumentation_client_native STATIC "${tracy_0131_SOURCE_DIR}/public/TracyClient.cpp")
target_compile_features(tracy_instrumentation_client_native PUBLIC cxx_std_20)
tracy_native_configure_client(tracy_instrumentation_client_native FALSE)
target_include_directories(
    tracy_instrumentation_client_native SYSTEM PUBLIC "${tracy_0131_SOURCE_DIR}/public"
)
target_link_libraries(tracy_instrumentation_client_native PUBLIC Threads::Threads ${CMAKE_DL_LIBS})

# Complete Tracy client and server in one library for the opt-in embedded
# capture backend. TracyClient.cpp supplies common system, LZ4, socket, and
# stack-frame symbols; only server-specific implementation files are compiled
# again here to avoid duplicate definitions.
add_library(
    tracy_embedded_capture_native STATIC
    "${tracy_0131_SOURCE_DIR}/public/TracyClient.cpp"
    "${tracy_0131_SOURCE_DIR}/public/common/tracy_lz4hc.cpp"
    "${tracy_0131_SOURCE_DIR}/server/TracyMemory.cpp"
    "${tracy_0131_SOURCE_DIR}/server/TracyMmap.cpp"
    "${tracy_0131_SOURCE_DIR}/server/TracyPrint.cpp"
    "${tracy_0131_SOURCE_DIR}/server/TracySysUtil.cpp"
    "${tracy_0131_SOURCE_DIR}/server/TracyTaskDispatch.cpp"
    "${tracy_0131_SOURCE_DIR}/server/TracyTextureCompression.cpp"
    "${tracy_0131_SOURCE_DIR}/server/TracyThreadCompress.cpp"
    "${tracy_0131_SOURCE_DIR}/server/TracyWorker.cpp"
    src/embedded/TracyEmbeddedTransport.cpp
    src/embedded/embedded_capture.cpp
)
add_library(TracyQuery::EmbeddedCapture ALIAS tracy_embedded_capture_native)
target_compile_features(tracy_embedded_capture_native PUBLIC cxx_std_20)
tracy_native_configure_client(tracy_embedded_capture_native TRUE)
target_compile_definitions(tracy_embedded_capture_native PUBLIC TRACY_EMBEDDED_CAPTURE TRACY_NO_BROADCAST)
target_include_directories(tracy_embedded_capture_native PUBLIC include)
target_include_directories(
    tracy_embedded_capture_native SYSTEM
    PUBLIC
        "${tracy_0131_SOURCE_DIR}/public"
        "${tracy_0131_SOURCE_DIR}/public/common"
        "${tracy_0131_SOURCE_DIR}/server"
    PRIVATE
        "${tracy_capstone_SOURCE_DIR}/include/capstone"
        "${tracy_zstd_SOURCE_DIR}/lib"
        "${tracy_ppqsort_SOURCE_DIR}/include"
)
target_link_libraries(
    tracy_embedded_capture_native
    PUBLIC capstone_static libzstd_static tracy_ppqsort_headers Threads::Threads ${CMAKE_DL_LIBS}
)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(tracy_embedded_capture_native PRIVATE -fno-sanitize=enum)
endif()

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
    add_executable(
        tracy-embedded-transport-unit-tests
        tests/embedded_transport_unit.cpp
        src/embedded/TracyEmbeddedTransport.cpp
    )
    target_compile_features(tracy-embedded-transport-unit-tests PRIVATE cxx_std_20)
    target_include_directories(tracy-embedded-transport-unit-tests PRIVATE src/embedded)
    target_link_libraries(tracy-embedded-transport-unit-tests PRIVATE Threads::Threads)
    tracy_query_apply_warnings(tracy-embedded-transport-unit-tests)
    add_test(NAME embedded-transport-unit COMMAND tracy-embedded-transport-unit-tests)
    set_tests_properties(embedded-transport-unit PROPERTIES TIMEOUT 30)

    add_executable(tracy-embedded-capture-fixture tests/fixtures/tracy_embedded_fixture.cpp)
    target_link_libraries(tracy-embedded-capture-fixture PRIVATE TracyQuery::EmbeddedCapture)
    tracy_query_apply_warnings(tracy-embedded-capture-fixture)
    add_test(
        NAME embedded-capture-native
        COMMAND ${CMAKE_COMMAND}
                -DFIXTURE=$<TARGET_FILE:tracy-embedded-capture-fixture>
                -DTRACY_QUERY=$<TARGET_FILE:tracy-query>
                -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/embedded-native.tracy
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/verify_embedded_capture.cmake
    )
    set_tests_properties(embedded-capture-native PROPERTIES TIMEOUT 60)
    add_test(
        NAME embedded-capture-existing-at-finish
        COMMAND tracy-embedded-capture-fixture
                ${CMAKE_CURRENT_BINARY_DIR}/embedded-existing-at-finish.tracy
                existing-at-finish
    )
    add_test(
        NAME embedded-capture-writer-error
        COMMAND tracy-embedded-capture-fixture
                ${CMAKE_CURRENT_BINARY_DIR}/missing-embedded-parent/capture.tracy
                expect-io-error
    )
    set_tests_properties(
        embedded-capture-existing-at-finish embedded-capture-writer-error
        PROPERTIES TIMEOUT 60
    )

    add_executable(tracy-embedded-capture-errors tests/embedded_capture_errors.cpp)
    target_link_libraries(tracy-embedded-capture-errors PRIVATE TracyQuery::EmbeddedCapture)
    tracy_query_apply_warnings(tracy-embedded-capture-errors)
    foreach(_mode IN ITEMS invalid existing duplicate no-data)
        add_test(
            NAME embedded-capture-error-${_mode}
            COMMAND tracy-embedded-capture-errors ${_mode}
                    ${CMAKE_CURRENT_BINARY_DIR}/embedded-error-${_mode}.tracy
        )
        set_tests_properties(embedded-capture-error-${_mode} PROPERTIES TIMEOUT 30)
    endforeach()

    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    find_program(CARGO_EXECUTABLE NAMES cargo)
    if(CARGO_EXECUTABLE)
        add_test(
            NAME embedded-capture-rust-normal
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/examples/rust-embedded-capture/run_demo.py
                    --mode normal
                    --output ${CMAKE_CURRENT_BINARY_DIR}/rust-embedded-normal.tracy
                    --query $<TARGET_FILE:tracy-query>
                    --target-dir ${CMAKE_CURRENT_BINARY_DIR}/rust-embedded-target
        )
        add_test(
            NAME embedded-capture-rust-panic
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/examples/rust-embedded-capture/run_demo.py
                    --mode unwind-panic
                    --output ${CMAKE_CURRENT_BINARY_DIR}/rust-embedded-panic.tracy
                    --query $<TARGET_FILE:tracy-query>
                    --target-dir ${CMAKE_CURRENT_BINARY_DIR}/rust-embedded-target
        )
        set_tests_properties(
            embedded-capture-rust-normal embedded-capture-rust-panic
            PROPERTIES
                TIMEOUT 900
                RUN_SERIAL TRUE
                ENVIRONMENT "TRACY_QUERY_CMAKE_BUILD_DIR=${CMAKE_BINARY_DIR}"
        )
        set_tests_properties(embedded-capture-rust-panic PROPERTIES DEPENDS embedded-capture-rust-normal)
    else()
        message(STATUS "cargo not found; Rust embedded-capture examples are not added to CTest")
    endif()

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
        PUBLIC
            TRACY_ENABLE TRACY_ON_DEMAND TRACY_DELAYED_INIT
            TRACY_ONLY_LOCALHOST TRACY_ONLY_IPV4 TRACY_TIMER_FALLBACK
            TRACY_NO_SYSTEM_TRACING TRACY_NO_CONTEXT_SWITCH TRACY_NO_SAMPLING
            TRACY_NO_CODE_TRANSFER TRACY_NO_BROADCAST TRACY_NO_CALLSTACK_INLINES
            TRACY_NO_CRASH_HANDLER
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

    add_executable(
        tracy-loopback-control-fixture
        tests/fixtures/tracy_loopback_control_fixture.cpp
    )
    target_link_libraries(
        tracy-loopback-control-fixture
        PRIVATE tracy_collector_tracy_client TracyQuery::TracyServer
    )
    add_test(
        NAME embedded-capture-loopback-control
        COMMAND ${CMAKE_COMMAND}
                -DFIXTURE=$<TARGET_FILE:tracy-loopback-control-fixture>
                -DTRACY_QUERY=$<TARGET_FILE:tracy-query>
                -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/embedded-loopback-control.tracy
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/verify_embedded_capture.cmake
    )
    set_tests_properties(
        embedded-capture-loopback-control
        PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE ENVIRONMENT "TRACY_PORT=61731"
    )

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
    set_tests_properties(collector-protocol-live PROPERTIES TIMEOUT 180)

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
