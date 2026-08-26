if(NOT DEFINED CXX_DEAD_EXECUTABLE OR NOT DEFINED CXX_DEAD_GOLDEN_FIXTURE OR
   NOT DEFINED CXX_DEAD_FAILURE_DIR)
    message(FATAL_ERROR "CLI failure test requires executable, fixture, and output directory")
endif()

file(MAKE_DIRECTORY "${CXX_DEAD_FAILURE_DIR}")
set(REPORT "${CXX_DEAD_FAILURE_DIR}/incomplete.json")
set(GRAPH "${CXX_DEAD_FAILURE_DIR}/partial.graph.json")
file(REMOVE "${REPORT}" "${GRAPH}")

execute_process(
    COMMAND "${CXX_DEAD_EXECUTABLE}"
        "${CXX_DEAD_GOLDEN_FIXTURE}/compile_commands.json"
        --project-root "${CXX_DEAD_GOLDEN_FIXTURE}"
        --format json
        --output "${REPORT}"
        --graph-output "${GRAPH}"
        --max-ast-bytes 128
    RESULT_VARIABLE RESULT
    ERROR_VARIABLE STANDARD_ERROR
)
if(NOT RESULT EQUAL 1)
    message(FATAL_ERROR "bounded incomplete run returned ${RESULT}: ${STANDARD_ERROR}")
endif()
if(NOT EXISTS "${REPORT}")
    message(FATAL_ERROR "bounded incomplete run did not write JSON diagnostics")
endif()
if(EXISTS "${GRAPH}")
    message(FATAL_ERROR "bounded incomplete run wrote a partial graph artifact")
endif()

file(READ "${REPORT}" DOCUMENT)
string(JSON STATE GET "${DOCUMENT}" run state)
if(NOT STATE STREQUAL "incomplete")
    message(FATAL_ERROR "bounded incomplete run reported state ${STATE}")
endif()
string(JSON FINDINGS_VALUE ERROR_VARIABLE FINDINGS_ERROR GET "${DOCUMENT}" findings)
if(FINDINGS_ERROR STREQUAL "NOTFOUND")
    message(FATAL_ERROR "bounded incomplete run emitted a findings field")
endif()
