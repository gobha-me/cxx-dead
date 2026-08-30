if(NOT DEFINED CXX_DEAD_EXECUTABLE OR NOT DEFINED CXX_DEAD_TARGET_FIXTURE_SOURCE OR
   NOT DEFINED CXX_DEAD_TARGET_FIXTURE_BUILD OR NOT DEFINED CXX_DEAD_DIFF_POLICY OR
   NOT DEFINED CXX_DEAD_WORK_DIR)
    message(FATAL_ERROR
        "agent workflow test requires executable, fixture, policy, and work directory"
    )
endif()

file(MAKE_DIRECTORY "${CXX_DEAD_WORK_DIR}")
set(BASELINE_GRAPH "${CXX_DEAD_WORK_DIR}/baseline.graph.json")
set(BASELINE_REPORT "${CXX_DEAD_WORK_DIR}/baseline.report.json")
set(CURRENT_GRAPH "${CXX_DEAD_WORK_DIR}/current.graph.json")
set(CURRENT_REPORT "${CXX_DEAD_WORK_DIR}/current.report.json")

execute_process(
    COMMAND "${CXX_DEAD_EXECUTABLE}"
        --cmake-build-dir "${CXX_DEAD_TARGET_FIXTURE_BUILD}"
        --configuration Debug
        --target production_app
        --configuration-id agent-workflow-debug
        --report-path "${CXX_DEAD_TARGET_FIXTURE_SOURCE}"
        --tu-timeout 120
        --index-timeout 900
        --max-ast-bytes 2147483648
        --cache-dir "${CXX_DEAD_WORK_DIR}/cache"
        --verbose
        --format json
        --output "${BASELINE_REPORT}"
        --graph-output "${BASELINE_GRAPH}"
    RESULT_VARIABLE BASELINE_RESULT
    ERROR_VARIABLE BASELINE_ERROR
)
if(NOT BASELINE_RESULT EQUAL 0)
    message(FATAL_ERROR "agent baseline failed (${BASELINE_RESULT}): ${BASELINE_ERROR}")
endif()

execute_process(
    COMMAND "${CXX_DEAD_EXECUTABLE}"
        --cmake-build-dir "${CXX_DEAD_TARGET_FIXTURE_BUILD}"
        --configuration Debug
        --target production_app
        --configuration-id agent-workflow-debug
        --report-path "${CXX_DEAD_TARGET_FIXTURE_SOURCE}"
        --tu-timeout 120
        --index-timeout 900
        --max-ast-bytes 2147483648
        --cache-dir "${CXX_DEAD_WORK_DIR}/cache"
        --verbose
        --baseline-graph "${BASELINE_GRAPH}"
        --diff-policy "${CXX_DEAD_DIFF_POLICY}"
        --fail-on-diff
        --format json
        --output "${CURRENT_REPORT}"
        --graph-output "${CURRENT_GRAPH}"
    RESULT_VARIABLE CURRENT_RESULT
    ERROR_VARIABLE CURRENT_ERROR
)
if(NOT CURRENT_RESULT EQUAL 0)
    message(FATAL_ERROR "unchanged agent comparison failed (${CURRENT_RESULT}): ${CURRENT_ERROR}")
endif()

file(READ "${CURRENT_REPORT}" CURRENT_JSON)
if(NOT CURRENT_JSON MATCHES "\"configuration_id\": \"agent-workflow-debug\"" OR
   NOT CURRENT_JSON MATCHES "\"target_name\": \"production_app\"" OR
   NOT CURRENT_JSON MATCHES "\"policy_matches\": 0")
    message(FATAL_ERROR
        "agent comparison omitted its target context or clean policy result: ${CURRENT_JSON}"
    )
endif()
