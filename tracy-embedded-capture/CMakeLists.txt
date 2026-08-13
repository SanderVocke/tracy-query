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
    "${tracy_0131_SOURCE_DIR}/public/common/TracyStackFrames.cpp"
    "${tracy_0131_SOURCE_DIR}/public/common/tracy_lz4hc.cpp"
    "${tracy_0131_SOURCE_DIR}/server/TracyMemory.cpp"
    "${tracy_0131_SOURCE_DIR}/server/TracyMmap.cpp"
    "${tracy_0131_SOURCE_DIR}/server/TracyPrint.cpp"
    "${tracy_0131_SOURCE_DIR}/server/TracySysUtil.cpp"
    "${tracy_0131_SOURCE_DIR}/server/TracyTaskDispatch.cpp"
    "${tracy_0131_SOURCE_DIR}/server/TracyTextureCompression.cpp"
    "${tracy_0131_SOURCE_DIR}/server/TracyThreadCompress.cpp"
    "${tracy_0131_SOURCE_DIR}/server/TracyWorker.cpp"
    ${CMAKE_CURRENT_SOURCE_DIR}/src/TracyEmbeddedTransport.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/embedded_capture.cpp
)
add_library(TracyQuery::EmbeddedCapture ALIAS tracy_embedded_capture_native)
target_compile_features(tracy_embedded_capture_native PUBLIC cxx_std_20)
tracy_native_configure_client(tracy_embedded_capture_native TRUE)
target_compile_definitions(tracy_embedded_capture_native PUBLIC TRACY_EMBEDDED_CAPTURE TRACY_NO_BROADCAST)
target_include_directories(tracy_embedded_capture_native PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
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
if(BUILD_TESTING)
    target_compile_definitions(tracy_embedded_capture_native PRIVATE TRACY_EMBEDDED_CAPTURE_TESTING=1)
endif()


if(BUILD_TESTING)
    add_executable(
        tracy-embedded-transport-unit-tests
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/embedded_transport_unit.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/TracyEmbeddedTransport.cpp
    )
    target_compile_features(tracy-embedded-transport-unit-tests PRIVATE cxx_std_20)
    target_include_directories(tracy-embedded-transport-unit-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(tracy-embedded-transport-unit-tests PRIVATE Threads::Threads)
    tracy_extensions_apply_warnings(tracy-embedded-transport-unit-tests)
    foreach(_mode IN ITEMS core peer-destroy cancel)
        add_test(
            NAME embedded-transport-${_mode}
            COMMAND tracy-embedded-transport-unit-tests ${_mode}
        )
        set_tests_properties(embedded-transport-${_mode} PROPERTIES TIMEOUT 30)
    endforeach()

    add_executable(tracy-embedded-capture-fixture ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/tracy_embedded_fixture.cpp)
    target_link_libraries(tracy-embedded-capture-fixture PRIVATE TracyQuery::EmbeddedCapture)
    tracy_extensions_apply_warnings(tracy-embedded-capture-fixture)
    add_test(
        NAME embedded-capture-native
        COMMAND ${CMAKE_COMMAND}
                -DFIXTURE=$<TARGET_FILE:tracy-embedded-capture-fixture>
                -DTRACY_QUERY=$<TARGET_FILE:tracy-query>
                -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/embedded-native.tracy
                -DREQUIRE_CALLSTACK=ON
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/verify_embedded_capture.cmake
    )
    set_tests_properties(embedded-capture-native PROPERTIES TIMEOUT 60)
    add_test(
        NAME embedded-capture-discard
        COMMAND tracy-embedded-capture-fixture
                ${CMAKE_CURRENT_BINARY_DIR}/embedded-discard.tracy
                discard
    )
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
    add_test(
        NAME embedded-capture-writer-open-failure
        COMMAND tracy-embedded-capture-fixture
                ${CMAKE_CURRENT_BINARY_DIR}/embedded-writer-open-failure.tracy
                writer-open-failure
    )
    add_test(
        NAME embedded-capture-finish-timeout
        COMMAND tracy-embedded-capture-fixture
                ${CMAKE_CURRENT_BINARY_DIR}/embedded-finish-timeout.tracy
                finish-timeout
    )
    set_tests_properties(
        embedded-capture-discard embedded-capture-existing-at-finish
        embedded-capture-writer-error embedded-capture-writer-open-failure
        embedded-capture-finish-timeout
        PROPERTIES TIMEOUT 60
    )

    add_executable(tracy-embedded-capture-errors ${CMAKE_CURRENT_SOURCE_DIR}/tests/embedded_capture_errors.cpp)
    target_link_libraries(tracy-embedded-capture-errors PRIVATE TracyQuery::EmbeddedCapture)
    tracy_extensions_apply_warnings(tracy-embedded-capture-errors)
    foreach(_mode IN ITEMS invalid existing duplicate invalid-disposition no-data)
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

    endif()

    add_library(
        tracy_loopback_control_client STATIC
        "${tracy_0131_SOURCE_DIR}/public/TracyClient.cpp"
    )
    target_compile_features(tracy_loopback_control_client PUBLIC cxx_std_20)
    target_compile_definitions(
        tracy_loopback_control_client
        PUBLIC
            TRACY_ENABLE TRACY_ON_DEMAND TRACY_DELAYED_INIT
            TRACY_ONLY_LOCALHOST TRACY_ONLY_IPV4 TRACY_TIMER_FALLBACK
            TRACY_NO_SYSTEM_TRACING TRACY_NO_CONTEXT_SWITCH TRACY_NO_SAMPLING
            TRACY_NO_CODE_TRANSFER TRACY_NO_BROADCAST TRACY_NO_CALLSTACK_INLINES
            TRACY_NO_CRASH_HANDLER
    )
    target_include_directories(
        tracy_loopback_control_client
        SYSTEM PUBLIC "${tracy_0131_SOURCE_DIR}/public"
    )
    target_link_libraries(
        tracy_loopback_control_client
        PUBLIC Threads::Threads ${CMAKE_DL_LIBS}
    )
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(tracy_loopback_control_client PRIVATE -fno-sanitize=enum)
    endif()

    add_executable(
        tracy-loopback-control-fixture
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/tracy_loopback_control_fixture.cpp
    )
    target_link_libraries(
        tracy-loopback-control-fixture
        PRIVATE tracy_loopback_control_client TracyQuery::TracyServer
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

endif()
