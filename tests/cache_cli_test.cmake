file(REMOVE_RECURSE "${CXX_DEAD_CACHE_DIR}")
file(REMOVE_RECURSE "${CXX_DEAD_DEFAULT_WORKSPACE}")
file(REMOVE "${CXX_DEAD_COLD_REPORT}" "${CXX_DEAD_WARM_REPORT}")

file(COPY "${CXX_DEAD_FIXTURE}/" DESTINATION "${CXX_DEAD_DEFAULT_WORKSPACE}")
execute_process(
    COMMAND "${CXX_DEAD_EXECUTABLE}"
        "${CXX_DEAD_DEFAULT_WORKSPACE}/compile_commands.json"
        --project-root "${CXX_DEAD_DEFAULT_WORKSPACE}"
        --format json
    RESULT_VARIABLE DEFAULT_RESULT
    OUTPUT_QUIET
    ERROR_VARIABLE DEFAULT_ERROR
)
if(NOT DEFAULT_RESULT EQUAL 0)
    message(FATAL_ERROR "Default cache run failed (${DEFAULT_RESULT}):\n${DEFAULT_ERROR}")
endif()
if(NOT EXISTS "${CXX_DEAD_DEFAULT_WORKSPACE}/.cxx-dead/cache/v1")
    message(FATAL_ERROR "Default project-local cache was not created")
endif()

execute_process(
    COMMAND "${CXX_DEAD_EXECUTABLE}"
        "${CXX_DEAD_FIXTURE}/compile_commands.json"
        --project-root "${CXX_DEAD_FIXTURE}"
        --cache-dir "${CXX_DEAD_CACHE_DIR}"
        --format json
        --output "${CXX_DEAD_COLD_REPORT}"
        --verbose
    RESULT_VARIABLE COLD_RESULT
    ERROR_VARIABLE COLD_ERROR
)
if(NOT COLD_RESULT EQUAL 0)
    message(FATAL_ERROR "Cold cache run failed (${COLD_RESULT}):\n${COLD_ERROR}")
endif()
if(NOT COLD_ERROR MATCHES "indexed_tus=3 reused_tus=0 cache_misses=3")
    message(FATAL_ERROR "Cold cache metrics were incomplete:\n${COLD_ERROR}")
endif()

execute_process(
    COMMAND "${CXX_DEAD_EXECUTABLE}"
        "${CXX_DEAD_FIXTURE}/compile_commands.json"
        --project-root "${CXX_DEAD_FIXTURE}"
        --cache-dir "${CXX_DEAD_CACHE_DIR}"
        --format json
        --output "${CXX_DEAD_WARM_REPORT}"
        --verbose
    RESULT_VARIABLE WARM_RESULT
    ERROR_VARIABLE WARM_ERROR
)
if(NOT WARM_RESULT EQUAL 0)
    message(FATAL_ERROR "Warm cache run failed (${WARM_RESULT}):\n${WARM_ERROR}")
endif()
if(NOT WARM_ERROR MATCHES "indexed_tus=0 reused_tus=3 cache_misses=0")
    message(FATAL_ERROR "Warm cache metrics did not report full reuse:\n${WARM_ERROR}")
endif()
if(NOT WARM_ERROR MATCHES
   "indexing_ms=[0-9]+ merging_ms=[0-9]+ traversal_ms=[0-9]+ scc_ms=[0-9]+ reporting_ms=[0-9]+")
    message(FATAL_ERROR "Stage telemetry fields were missing:\n${WARM_ERROR}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E compare_files
        "${CXX_DEAD_COLD_REPORT}" "${CXX_DEAD_WARM_REPORT}"
    RESULT_VARIABLE COMPARE_RESULT
)
if(NOT COMPARE_RESULT EQUAL 0)
    message(FATAL_ERROR "Cold and warm JSON reports differ")
endif()
