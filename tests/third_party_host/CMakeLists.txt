cmake_minimum_required(VERSION 3.20)
project(workshop_agent_host LANGUAGES CXX)

find_package(HuxerUI CONFIG REQUIRED COMPONENTS testing)

set(AGENTKIT_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../.." CACHE PATH "AgentKit library source")
add_executable(workshop_agent_host main.cpp)
huxerui_use_library(workshop_agent_host TARGET HuxerUI::AgentKit PATH "${AGENTKIT_SOURCE_DIR}")
target_link_libraries(workshop_agent_host PRIVATE HuxerUI::testing)
target_include_directories(workshop_agent_host PRIVATE "${AGENTKIT_SOURCE_DIR}/tests")
set_property(TARGET workshop_agent_host APPEND PROPERTY HUXERUI_RESOURCE_PACKAGE_INPUTS
        "${HUXERUI_BUILTIN_RESOURCE_PACKAGE}")
target_compile_definitions(workshop_agent_host PRIVATE
        AGENTKIT_TEST_RESOURCE_PACKAGE="$<TARGET_PROPERTY:workshop_agent_host,HUXERUI_RESOURCE_PACKAGE>")

enable_testing()
add_test(NAME workshop_agent_host COMMAND workshop_agent_host)
set_tests_properties(workshop_agent_host PROPERTIES TIMEOUT 90)

option(HUXERUI_AGENTKIT_BUILD_SSH_TASK_HOST "Build the desktop SSH task host example" OFF)
if (HUXERUI_AGENTKIT_BUILD_SSH_TASK_HOST)
    add_executable(ssh_task_host ssh_task_host.cpp)
    huxerui_use_library(ssh_task_host TARGET HuxerUI::AgentKit PATH "${AGENTKIT_SOURCE_DIR}")
    include("${AGENTKIT_SOURCE_DIR}/cmake/HostTerminal.cmake")
    agentkit_host_link_terminal(ssh_task_host)
    target_link_libraries(ssh_task_host PRIVATE HuxerUI::testing)

    find_package(Python3 QUIET COMPONENTS Interpreter)
    find_program(OPENSSH_CLIENT NAMES ssh)
    if (Python3_Interpreter_FOUND AND OPENSSH_CLIENT)
        add_test(NAME ssh_task_host COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/ssh_task_fixture.py" "$<TARGET_FILE:ssh_task_host>" "${OPENSSH_CLIENT}")
        set_tests_properties(ssh_task_host PROPERTIES TIMEOUT 100 SKIP_RETURN_CODE 77)
    endif ()
endif ()
