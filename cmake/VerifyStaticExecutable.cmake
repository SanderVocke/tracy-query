if(NOT DEFINED TRACY_QUERY_EXECUTABLE OR NOT EXISTS "${TRACY_QUERY_EXECUTABLE}")
    message(FATAL_ERROR "TRACY_QUERY_EXECUTABLE must name the built executable")
endif()
if(NOT DEFINED TRACY_QUERY_READELF OR NOT EXISTS "${TRACY_QUERY_READELF}")
    message(FATAL_ERROR "CMake did not provide a usable readelf tool")
endif()

execute_process(
    COMMAND "${TRACY_QUERY_READELF}" -d "${TRACY_QUERY_EXECUTABLE}"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE dynamic_section
    ERROR_VARIABLE error
)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "Unable to inspect ${TRACY_QUERY_EXECUTABLE}: ${error}")
endif()
if(dynamic_section MATCHES "\\(NEEDED\\)")
    message(FATAL_ERROR
        "${TRACY_QUERY_EXECUTABLE} has dynamic dependencies:\n${dynamic_section}"
    )
endif()

message(STATUS "Verified fully static executable: ${TRACY_QUERY_EXECUTABLE}")
