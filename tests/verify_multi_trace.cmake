if(NOT DEFINED TRACY_QUERY OR NOT DEFINED REFERENCE_TRACE OR NOT DEFINED SYNTHETIC_TRACE)
    message(FATAL_ERROR "TRACY_QUERY, REFERENCE_TRACE, and SYNTHETIC_TRACE are required")
endif()

execute_process(
    COMMAND "${TRACY_QUERY}" query --kind message --count --group-by trace
            "${REFERENCE_TRACE}" "${SYNTHETIC_TRACE}"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "multi-trace query failed: ${error}")
endif()
foreach(expected
        "\"trace\":\"${REFERENCE_TRACE}\",\"count\":7245"
        "\"trace\":\"${SYNTHETIC_TRACE}\",\"count\":2")
    string(FIND "${output}" "${expected}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "missing ${expected} in multi-trace output: ${output}")
    endif()
endforeach()
