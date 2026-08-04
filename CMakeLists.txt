cmake_minimum_required(VERSION 3.24)

project(tracy-query VERSION 0.1.0 LANGUAGES C CXX)

include(cmake/TracyServer.cmake)

add_executable(tracy-query src/main.cpp)
target_compile_features(tracy-query PRIVATE cxx_std_20)
target_link_libraries(tracy-query PRIVATE TracyQuery::TracyServer)

if(MSVC)
    target_compile_options(tracy-query PRIVATE /W4 /permissive-)
else()
    target_compile_options(tracy-query PRIVATE -Wall -Wextra -Wpedantic)
endif()

include(GNUInstallDirs)
install(TARGETS tracy-query RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

include(CTest)
if(BUILD_TESTING)
    add_test(NAME cli-help COMMAND tracy-query --help)

    add_test(NAME cli-requires-trace-file COMMAND tracy-query)
    set_tests_properties(cli-requires-trace-file PROPERTIES WILL_FAIL TRUE)

    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/not-a-trace.tracy" "not a Tracy trace\n")
    add_test(
        NAME cli-rejects-invalid-trace
        COMMAND tracy-query "${CMAKE_CURRENT_BINARY_DIR}/not-a-trace.tracy"
    )
    set_tests_properties(cli-rejects-invalid-trace PROPERTIES WILL_FAIL TRUE)
endif()
