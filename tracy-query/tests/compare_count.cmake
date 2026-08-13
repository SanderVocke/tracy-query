if(NOT DEFINED TRACY_QUERY OR NOT DEFINED TRACE_FILE)
    message(FATAL_ERROR "TRACY_QUERY and TRACE_FILE are required")
endif()

execute_process(
    COMMAND "${TRACY_QUERY}" query --kind message --from 1s --to 1.1s "${TRACE_FILE}"
    RESULT_VARIABLE normal_status
    OUTPUT_VARIABLE normal_output
    ERROR_VARIABLE normal_error
)
if(NOT normal_status EQUAL 0)
    message(FATAL_ERROR "normal query failed: ${normal_error}")
endif()
string(REGEX MATCHALL "\n" normal_lines "${normal_output}")
list(LENGTH normal_lines normal_count)

execute_process(
    COMMAND "${TRACY_QUERY}" query --kind message --from 1s --to 1.1s --count "${TRACE_FILE}"
    RESULT_VARIABLE count_status
    OUTPUT_VARIABLE count_output
    ERROR_VARIABLE count_error
)
if(NOT count_status EQUAL 0)
    message(FATAL_ERROR "count query failed: ${count_error}")
endif()
string(REGEX MATCH "\"count\":([0-9]+)" unused "${count_output}")
set(reported_count "${CMAKE_MATCH_1}")
if(NOT normal_count EQUAL reported_count)
    message(FATAL_ERROR "normal output had ${normal_count} records but --count reported ${reported_count}")
endif()
