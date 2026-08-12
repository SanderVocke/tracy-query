if(NOT DEFINED FIXTURE OR NOT DEFINED TRACY_QUERY OR NOT DEFINED OUTPUT_FILE)
    message(FATAL_ERROR "FIXTURE, TRACY_QUERY, and OUTPUT_FILE are required")
endif()

file(REMOVE "${OUTPUT_FILE}")
file(GLOB _partials "${OUTPUT_FILE}.*.partial")
if(_partials)
    file(REMOVE ${_partials})
endif()

execute_process(COMMAND "${FIXTURE}" "${OUTPUT_FILE}"
                RESULT_VARIABLE _fixture_result
                OUTPUT_VARIABLE _fixture_stdout
                ERROR_VARIABLE _fixture_stderr
                TIMEOUT 30)
if(NOT _fixture_result EQUAL 0)
    message(FATAL_ERROR "embedded fixture failed (${_fixture_result})\n${_fixture_stdout}\n${_fixture_stderr}")
endif()

execute_process(COMMAND "${TRACY_QUERY}" check "${OUTPUT_FILE}"
                RESULT_VARIABLE _check_result
                OUTPUT_VARIABLE _check_stdout
                ERROR_VARIABLE _check_stderr)
if(NOT _check_result EQUAL 0)
    message(FATAL_ERROR "embedded capture check failed\n${_check_stdout}\n${_check_stderr}")
endif()

foreach(_kind IN ITEMS cpu-zone message plot frame lock memory)
    execute_process(
        COMMAND "${TRACY_QUERY}" query --kind "${_kind}" --count "${OUTPUT_FILE}"
        RESULT_VARIABLE _query_result
        OUTPUT_VARIABLE _query_stdout
        ERROR_VARIABLE _query_stderr)
    if(NOT _query_result EQUAL 0 OR NOT _query_stdout MATCHES "\"count\":[1-9][0-9]*")
        message(FATAL_ERROR "missing ${_kind} records\n${_query_stdout}\n${_query_stderr}")
    endif()
endforeach()

file(GLOB _partials "${OUTPUT_FILE}.*.partial")
if(_partials)
    message(FATAL_ERROR "embedded capture left partial files: ${_partials}")
endif()
