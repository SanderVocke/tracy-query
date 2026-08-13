cmake_minimum_required(VERSION 3.24)

project(tracy-extensions VERSION 0.4.0 LANGUAGES C CXX)

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

option(TRACY_QUERY_STATIC_MSVC_RUNTIME "Use the static MSVC runtime" ON)
if(MSVC AND TRACY_QUERY_STATIC_MSVC_RUNTIME)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build dependencies as static libraries" FORCE)

include(cmake/TracyServer.cmake)

option(TRACY_NATIVE_ON_DEMAND "Enable Tracy on-demand mode in native client libraries" ON)
option(TRACY_NATIVE_MANUAL_LIFETIME "Enable Tracy manual lifetime in ordinary client library" OFF)
option(TRACY_NATIVE_DELAYED_INIT "Enable Tracy delayed initialization in ordinary client library" OFF)
option(TRACY_NATIVE_FLUSH_ON_EXIT "Keep Tracy alive to flush on process exit" OFF)
option(TRACY_NATIVE_DEBUGINFOD "Enable Tracy debuginfod integration" OFF)
option(TRACY_NATIVE_ONLY_LOCALHOST "Restrict ordinary Tracy client sockets to localhost" OFF)
option(TRACY_NATIVE_ONLY_IPV4 "Restrict ordinary Tracy client sockets to IPv4" OFF)
option(TRACY_NATIVE_TIMER_FALLBACK "Use Tracy's timer fallback" OFF)
if(BUILD_TESTING AND APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    set(TRACY_NATIVE_TIMER_FALLBACK ON CACHE BOOL "Use Tracy's timer fallback" FORCE)
endif()
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

function(tracy_extensions_apply_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

function(tracy_extensions_configure_executable target)
    tracy_extensions_apply_warnings(${target})
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

include(GNUInstallDirs)
include(CTest)

add_subdirectory(tracy-embedded-capture)
add_subdirectory(tracy-query)
add_subdirectory(tracy-nextest-capture)
