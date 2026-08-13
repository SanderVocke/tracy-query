if(NOT DEFINED GENERATOR OR NOT DEFINED TRACY_QUERY OR NOT DEFINED OUTPUT_FILE)
    message(FATAL_ERROR "GENERATOR, TRACY_QUERY, and OUTPUT_FILE are required")
endif()

file(REMOVE "${OUTPUT_FILE}")
execute_process(
    COMMAND "${GENERATOR}" "${OUTPUT_FILE}"
    RESULT_VARIABLE generator_status
    ERROR_VARIABLE generator_error
)
if(NOT generator_status EQUAL 0)
    message(FATAL_ERROR "structural fixture generator failed: ${generator_error}")
endif()

execute_process(
    COMMAND "${TRACY_QUERY}" query
            --kind gpu-zone,context-switch,cpu-slice,cpu-usage
            --count --group-by kind "${OUTPUT_FILE}"
    RESULT_VARIABLE query_status
    OUTPUT_VARIABLE query_output
    ERROR_VARIABLE query_error
)
if(NOT query_status EQUAL 0)
    message(FATAL_ERROR "generated structural query failed: ${query_error}")
endif()

foreach(expected
        "\"kind\":\"gpu-zone\",\"count\":1"
        "\"kind\":\"context-switch\",\"count\":1"
        "\"kind\":\"cpu-slice\",\"count\":1"
        "\"kind\":\"cpu-usage\",\"count\":3")
    string(FIND "${query_output}" "${expected}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "missing ${expected} in generated query output: ${query_output}")
    endif()
endforeach()

file(REMOVE "${OUTPUT_FILE}")
