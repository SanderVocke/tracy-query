if(NOT BUILD_TESTING)
    return()
endif()

find_package(Python3 COMPONENTS Interpreter REQUIRED)
find_program(CARGO_EXECUTABLE NAMES cargo)
set(CARGO_NEXTEST_IN_PROCESS_EXECUTABLE "" CACHE FILEPATH "cargo-nextest executable for in-process capture tests")

if(NOT CARGO_EXECUTABLE)
    message(STATUS "cargo not found; nextest capture tests are not added to CTest")
    return()
endif()

set(_helper_manifest "${CMAKE_CURRENT_SOURCE_DIR}/crates/tracy-nextest-capture/Cargo.toml")
set(_fixture_manifest "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixture/Cargo.toml")
set(_nextest_target "${CMAKE_CURRENT_BINARY_DIR}/target")

add_test(
    NAME nextest-in-process-helper-unit
    COMMAND ${CARGO_EXECUTABLE} test --manifest-path ${_helper_manifest} --locked
)
set_tests_properties(
    nextest-in-process-helper-unit
    PROPERTIES
        TIMEOUT 900
        RUN_SERIAL TRUE
        ENVIRONMENT "TRACY_QUERY_CMAKE_BUILD_DIR=${CMAKE_BINARY_DIR}"
)

if(NOT CARGO_NEXTEST_IN_PROCESS_EXECUTABLE)
    find_program(CARGO_NEXTEST_IN_PROCESS_EXECUTABLE NAMES cargo-nextest)
endif()
if(CARGO_NEXTEST_IN_PROCESS_EXECUTABLE)
    foreach(_policy IN ITEMS off failure always)
        add_test(
            NAME nextest-in-process-${_policy}
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/nextest_in_process.py
                    --nextest ${CARGO_NEXTEST_IN_PROCESS_EXECUTABLE}
                    --manifest ${_fixture_manifest}
                    --query $<TARGET_FILE:tracy-query>
                    --work ${CMAKE_CURRENT_BINARY_DIR}/${_policy}
                    --target-dir ${_nextest_target}
                    --policy ${_policy}
        )
        set_tests_properties(
            nextest-in-process-${_policy}
            PROPERTIES
                TIMEOUT 1800
                RUN_SERIAL TRUE
                ENVIRONMENT "TRACY_QUERY_CMAKE_BUILD_DIR=${CMAKE_BINARY_DIR}"
        )
    endforeach()
    add_test(
        NAME nextest-in-process-failure-modes
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/nextest_failure_modes.py
                --nextest ${CARGO_NEXTEST_IN_PROCESS_EXECUTABLE}
                --manifest ${_fixture_manifest}
                --work ${CMAKE_CURRENT_BINARY_DIR}/failure-modes
                --target-dir ${_nextest_target}
    )
    set_tests_properties(
        nextest-in-process-failure-modes
        PROPERTIES
            TIMEOUT 1800
            RUN_SERIAL TRUE
            ENVIRONMENT "TRACY_QUERY_CMAKE_BUILD_DIR=${CMAKE_BINARY_DIR}"
    )
else()
    message(STATUS "cargo-nextest not found; CI supplies the pinned executable")
endif()
