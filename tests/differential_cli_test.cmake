file(MAKE_DIRECTORY "${CXX_DEAD_WORK_DIR}")

set(BASELINE "${CXX_DEAD_WORK_DIR}/baseline.graph.json")
set(BASELINE_REPORT "${CXX_DEAD_WORK_DIR}/baseline.report.json")
execute_process(
    COMMAND "${CXX_DEAD_EXECUTABLE}"
        "${CXX_DEAD_PROVIDER_FIXTURE}/compile_commands.json"
        --project-root "${CXX_DEAD_PROVIDER_FIXTURE}"
        --ast-filter provider_fixture
        --root provider_fixture::run_registration
        --provider-config "${CXX_DEAD_PROVIDER_FIXTURE}/provider.yaml"
        --no-cache
        --format json
        --output "${BASELINE_REPORT}"
        --graph-output "${BASELINE}"
    RESULT_VARIABLE BASELINE_RESULT
    ERROR_VARIABLE BASELINE_ERROR
)
if(NOT BASELINE_RESULT EQUAL 0)
    message(FATAL_ERROR "baseline generation failed (${BASELINE_RESULT}): ${BASELINE_ERROR}")
endif()

set(DIFF_REPORT "${CXX_DEAD_WORK_DIR}/diff.json")
execute_process(
    COMMAND "${CXX_DEAD_EXECUTABLE}"
        "${CXX_DEAD_PROVIDER_FIXTURE}/compile_commands.json"
        --project-root "${CXX_DEAD_PROVIDER_FIXTURE}"
        --ast-filter provider_fixture
        --root provider_fixture::run_registration
        --no-cache
        --baseline-graph "${BASELINE}"
        --diff-policy "${CXX_DEAD_DIFF_POLICY}"
        --fail-on-diff
        --format json
        --output "${DIFF_REPORT}"
    RESULT_VARIABLE DIFF_RESULT
    ERROR_VARIABLE DIFF_ERROR
)
if(NOT DIFF_RESULT EQUAL 2)
    message(FATAL_ERROR "differential gate returned ${DIFF_RESULT}, expected 2: ${DIFF_ERROR}")
endif()
file(READ "${DIFF_REPORT}" DIFF_JSON)
if(NOT DIFF_JSON MATCHES "\"diff_schema_version\": 1" OR
   NOT DIFF_JSON MATCHES "\"newly_unreachable\": 4" OR
   NOT DIFF_JSON MATCHES "\"policy_matches\": 2")
    message(FATAL_ERROR "differential JSON omitted expected transitions: ${DIFF_JSON}")
endif()

set(SARIF_REPORT "${CXX_DEAD_WORK_DIR}/diff.sarif")
execute_process(
    COMMAND "${CXX_DEAD_EXECUTABLE}"
        "${CXX_DEAD_PROVIDER_FIXTURE}/compile_commands.json"
        --project-root "${CXX_DEAD_PROVIDER_FIXTURE}"
        --ast-filter provider_fixture
        --root provider_fixture::run_registration
        --no-cache
        --baseline-graph "${BASELINE}"
        --diff-policy "${CXX_DEAD_DIFF_POLICY}"
        --fail-on-diff
        --format sarif
        --output "${SARIF_REPORT}"
    RESULT_VARIABLE SARIF_RESULT
    ERROR_VARIABLE SARIF_ERROR
)
if(NOT SARIF_RESULT EQUAL 2)
    message(FATAL_ERROR "SARIF gate returned ${SARIF_RESULT}, expected 2: ${SARIF_ERROR}")
endif()
file(READ "${SARIF_REPORT}" SARIF_JSON)
if(NOT SARIF_JSON MATCHES "\"version\": \"2.1.0\"" OR
   NOT SARIF_JSON MATCHES "\"uri\": \"main.cpp\"" OR
   SARIF_JSON MATCHES "registered_callback is dynamically_referenced")
    message(FATAL_ERROR "SARIF did not contain only policy-matching current locations: ${SARIF_JSON}")
endif()

set(INVALID_BASELINE "${CXX_DEAD_WORK_DIR}/invalid.graph.json")
file(WRITE "${INVALID_BASELINE}" "{\"run\": {\"state\": \"incomplete\"}}\n")
execute_process(
    COMMAND "${CXX_DEAD_EXECUTABLE}"
        "${CXX_DEAD_PROVIDER_FIXTURE}/compile_commands.json"
        --project-root "${CXX_DEAD_PROVIDER_FIXTURE}"
        --ast-filter provider_fixture
        --root provider_fixture::run_registration
        --no-cache
        --baseline-graph "${INVALID_BASELINE}"
        --format json
    RESULT_VARIABLE INVALID_RESULT
    ERROR_VARIABLE INVALID_ERROR
)
if(NOT INVALID_RESULT EQUAL 1 OR NOT INVALID_ERROR MATCHES "invalid graph artifact")
    message(FATAL_ERROR "invalid baseline did not fail closed: ${INVALID_RESULT}: ${INVALID_ERROR}")
endif()
