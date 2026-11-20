file(REAL_PATH "${CMAKE_CURRENT_BINARY_DIR}" evidence_parent)
string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef nonce)
set(evidence "${evidence_parent}/editor-recovery-${nonce}")

foreach (stage prepare resume)
    execute_process(COMMAND "${PREVIEW_TEST}" "--code-editor-${stage}" "${evidence}"
            RESULT_VARIABLE status)
    if (NOT status STREQUAL "0")
        message(FATAL_ERROR "Editor recovery ${stage} failed with ${status}. Evidence: ${evidence}")
    endif ()
endforeach ()

file(REAL_PATH "${evidence}" resolved_evidence)
if (resolved_evidence STREQUAL evidence)
    file(REMOVE_RECURSE "${resolved_evidence}")
endif ()
