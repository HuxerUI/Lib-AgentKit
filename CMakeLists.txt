cmake_minimum_required(VERSION 3.20)

# CMake initializes Apple deployment defaults during the first project() call.
# Explicit settings and toolchains retain ownership of their target platform.
if (CMAKE_HOST_APPLE AND (NOT CMAKE_SYSTEM_NAME OR CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        AND NOT CMAKE_TOOLCHAIN_FILE AND "$ENV{CMAKE_TOOLCHAIN_FILE}" STREQUAL ""
        AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET AND "$ENV{MACOSX_DEPLOYMENT_TARGET}" STREQUAL "")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum macOS deployment target")
endif ()

project(agent_kit VERSION 0.1.0 LANGUAGES NONE)

include("${CMAKE_CURRENT_SOURCE_DIR}/HuxerUIProject.cmake")
if (HUXERUI_PROJECT_PLAN_ONLY)
    return()
endif ()

file(GLOB_RECURSE LIBRARY_SOURCE_FILES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cxx"
)
if (ANDROID)
    file(GLOB_RECURSE LIBRARY_ANDROID_SOURCE_FILES CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/android/src/main/cpp/*.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/android/src/main/cpp/*.cc"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/android/src/main/cpp/*.cxx"
    )
    list(APPEND LIBRARY_SOURCE_FILES ${LIBRARY_ANDROID_SOURCE_FILES})
endif ()
if (WIN32)
    file(GLOB_RECURSE LIBRARY_WINDOWS_SOURCE_FILES CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/windows/src/*.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/windows/src/*.cc"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/windows/src/*.cxx"
    )
    list(APPEND LIBRARY_SOURCE_FILES ${LIBRARY_WINDOWS_SOURCE_FILES})
endif ()
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    file(GLOB_RECURSE LIBRARY_LINUX_SOURCE_FILES CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/linux/src/*.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/linux/src/*.cc"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/linux/src/*.cxx"
    )
    list(APPEND LIBRARY_SOURCE_FILES ${LIBRARY_LINUX_SOURCE_FILES})
endif ()
if (APPLE AND NOT IOS)
    file(GLOB_RECURSE LIBRARY_MACOS_SOURCE_FILES CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/macos/src/*.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/macos/src/*.cc"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/macos/src/*.cxx"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/macos/src/*.mm"
    )
    list(APPEND LIBRARY_SOURCE_FILES ${LIBRARY_MACOS_SOURCE_FILES})
endif ()
if (EMSCRIPTEN)
    file(GLOB_RECURSE LIBRARY_WEB_SOURCE_FILES CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/web/src/*.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/web/src/*.cc"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/web/src/*.cxx"
    )
    list(APPEND LIBRARY_SOURCE_FILES ${LIBRARY_WEB_SOURCE_FILES})
endif ()

huxerui_add_library(huxerui_agentkit
        SOURCES
            ${LIBRARY_SOURCE_FILES}
        RESOURCES
            resources
        RESOURCE_NAMESPACE
            huxerui_agentkit
)

add_library(HuxerUI::AgentKit ALIAS huxerui_agentkit)

if (NOT TARGET HuxerUI::Markdown)
    cmake_policy(PUSH)
    if (POLICY CMP0168)
        cmake_policy(SET CMP0168 NEW)
    endif ()
    include(FetchContent)
    # Use a fresh checkout when upgrading from the locally patched dependency.
    FetchContent_Declare(huxerui_markdown
            GIT_REPOSITORY https://github.com/HuxerUI/Lib-Markdown.git
            GIT_TAG 3a004b8fb01adf81e46848d091b09909afd2e81b
    )
    FetchContent_MakeAvailable(huxerui_markdown)
    cmake_policy(POP)
    # huxerui_add_library already selects and links the SDK's static framework.
    # The pinned Markdown CMake also links the shared target explicitly.
    if (TARGET HuxerUI::huxerui_static AND NOT HUXERUI_LIBRARY_GRAPH_ONLY)
        get_target_property(agentkit_markdown_target HuxerUI::Markdown ALIASED_TARGET)
        foreach (link_property IN ITEMS LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
            get_property(markdown_links TARGET ${agentkit_markdown_target} PROPERTY ${link_property})
            list(REMOVE_ITEM markdown_links HuxerUI::huxerui)
            set_property(TARGET ${agentkit_markdown_target} PROPERTY ${link_property} ${markdown_links})
        endforeach ()
    endif ()
endif ()
huxerui_use_library(huxerui_agentkit TARGET HuxerUI::Markdown)

if (HUXERUI_LIBRARY_GRAPH_ONLY)
    return()
endif ()

target_include_directories(huxerui_agentkit PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:include>"
)
target_compile_features(huxerui_agentkit PUBLIC cxx_std_20)
if (MSVC)
    target_compile_options(huxerui_agentkit PRIVATE /bigobj)
endif ()

if (NOT TARGET nlohmann_json::nlohmann_json)
    find_package(nlohmann_json 3.12 QUIET)
endif ()
if (NOT TARGET nlohmann_json::nlohmann_json)
    include(FetchContent)
    if (POLICY CMP0135)
        cmake_policy(SET CMP0135 NEW)
    endif ()
    FetchContent_Declare(nlohmann_json
            URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
            URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
    )
    FetchContent_MakeAvailable(nlohmann_json)
endif ()
target_link_libraries(huxerui_agentkit PRIVATE nlohmann_json::nlohmann_json)

option(HUXERUI_AGENTKIT_BUILD_TESTS "Build AgentKit tests" OFF)
option(HUXERUI_AGENTKIT_BUILD_PREVIEW_TESTS "Include the Preview integration tests" ON)
if (HUXERUI_AGENTKIT_BUILD_TESTS)
    if (NOT TARGET HuxerUI::testing)
        find_package(HuxerUI CONFIG REQUIRED COMPONENTS testing PATHS "${HUXERUI_HOME}")
    endif ()
    enable_testing()
    add_executable(huxerui_agentkit_runtime_tests tests/runtime_test.cpp)
    huxerui_use_library(huxerui_agentkit_runtime_tests TARGET HuxerUI::AgentKit)
    target_link_libraries(huxerui_agentkit_runtime_tests PRIVATE HuxerUI::testing)
    set_property(TARGET huxerui_agentkit_runtime_tests APPEND PROPERTY HUXERUI_RESOURCE_PACKAGE_INPUTS
            "${HUXERUI_BUILTIN_RESOURCE_PACKAGE}")
    target_compile_definitions(huxerui_agentkit_runtime_tests PRIVATE
            AGENTKIT_TEST_RESOURCE_PACKAGE="$<TARGET_PROPERTY:huxerui_agentkit_runtime_tests,HUXERUI_RESOURCE_PACKAGE>")
    add_test(NAME huxerui_agentkit_runtime_tests COMMAND huxerui_agentkit_runtime_tests)
    add_test(NAME huxerui_agentkit_tool_schedule_tests COMMAND huxerui_agentkit_runtime_tests --tool-schedule)
    if (NOT CMAKE_CROSSCOMPILING AND (WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_SYSTEM_NAME STREQUAL "Darwin"))
        find_package(Python3 QUIET COMPONENTS Interpreter)
        if (Python3_Interpreter_FOUND)
            target_include_directories(huxerui_agentkit_runtime_tests PRIVATE src)
            target_compile_definitions(huxerui_agentkit_runtime_tests PRIVATE AGENTKIT_TEST_MCP_TRANSPORTS)
            target_link_libraries(huxerui_agentkit_runtime_tests PRIVATE nlohmann_json::nlohmann_json)
            if (MSVC)
                target_compile_options(huxerui_agentkit_runtime_tests PRIVATE /bigobj)
            endif ()
            add_test(NAME huxerui_agentkit_mcp_http_tests COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/mcp_http_server.py" $<TARGET_FILE:huxerui_agentkit_runtime_tests>)
            add_test(NAME huxerui_agentkit_mcp_stdio_tests COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/mcp_stdio_server.py" $<TARGET_FILE:huxerui_agentkit_runtime_tests>)
            set_tests_properties(huxerui_agentkit_mcp_http_tests huxerui_agentkit_mcp_stdio_tests PROPERTIES TIMEOUT 45)
        endif ()
    endif ()

    include(cmake/HostSQLite.cmake)
    add_executable(huxerui_agentkit_sqlite_tests
            tests/sqlite_store_test.cpp
            tests/fork_history_test.cpp
    )
    if (MSVC)
        target_compile_options(huxerui_agentkit_sqlite_tests PRIVATE /bigobj)
    endif ()
    target_link_libraries(huxerui_agentkit_sqlite_tests PRIVATE HuxerUI::AgentKit HuxerUI::testing nlohmann_json::nlohmann_json)
    agentkit_host_use_sqlite_session_store(huxerui_agentkit_sqlite_tests)
    add_test(NAME huxerui_agentkit_sqlite_tests COMMAND huxerui_agentkit_sqlite_tests)

    add_executable(huxerui_agentkit_delegation_tests tests/delegation_test.cpp)
    target_link_libraries(huxerui_agentkit_delegation_tests PRIVATE HuxerUI::AgentKit HuxerUI::testing)
    agentkit_host_use_sqlite_session_store(huxerui_agentkit_delegation_tests)
    add_test(NAME huxerui_agentkit_delegation_tests COMMAND huxerui_agentkit_delegation_tests)
    set_tests_properties(huxerui_agentkit_delegation_tests PROPERTIES TIMEOUT 45)

    add_executable(huxerui_agentkit_model_tests
            tests/model_protocol_test.cpp
    )
    target_include_directories(huxerui_agentkit_model_tests PRIVATE src)
    target_link_libraries(huxerui_agentkit_model_tests PRIVATE
            HuxerUI::AgentKit HuxerUI::testing nlohmann_json::nlohmann_json)
    agentkit_host_use_sqlite_session_store(huxerui_agentkit_model_tests)
    add_test(NAME huxerui_agentkit_model_tests COMMAND huxerui_agentkit_model_tests)
endif ()

if (HUXERUI_AGENTKIT_BUILD_TESTS AND HUXERUI_AGENTKIT_BUILD_PREVIEW_TESTS)
    include(examples/preview/cmake/Archive.cmake)
    include(cmake/HostTerminal.cmake)
    include(examples/preview/cmake/Skills.cmake)
    add_executable(huxerui_agentkit_preview_tests
            tests/preview_test.cpp
            examples/preview/src/app.cpp
            examples/preview/src/attachment_imports.cpp
            examples/preview/src/code_workspace.cpp
            examples/preview/src/code_tools.cpp
            examples/preview/src/code_tests.cpp
            examples/preview/src/code_workflow.cpp
            examples/preview/src/design.cpp
            examples/preview/src/design_view.cpp
            examples/preview/src/fork_review_view.cpp
            examples/preview/src/git_process.cpp
            examples/preview/src/goal_view.cpp
            examples/preview/src/general_workflow.cpp
            examples/preview/src/link_navigation.cpp
            examples/preview/src/markdown_images.cpp
            examples/preview/src/release_skill.cpp
            examples/preview/src/report_chart.cpp
            examples/preview/src/run_settings.cpp
            examples/preview/src/preview_settings.cpp
            examples/preview/src/release_workflow.cpp
            examples/preview/src/workspace_evidence.cpp
            examples/preview/src/worktree.cpp
            examples/preview/src/presentation.cpp
            examples/preview/src/presentation_tools.cpp
            examples/preview/src/presentation_view.cpp
            examples/preview/src/presentation_workflow.cpp
            examples/preview/src/sqlite_store.cpp
    )
    huxerui_enable_codegen(huxerui_agentkit_preview_tests)
    if (MSVC)
        target_compile_options(huxerui_agentkit_preview_tests PRIVATE /bigobj)
    endif ()
    agentkit_preview_embed_skills(huxerui_agentkit_preview_tests)
    huxerui_use_library(huxerui_agentkit_preview_tests TARGET HuxerUI::AgentKit)
    huxerui_add_resources(huxerui_agentkit_preview_tests ROOT "${CMAKE_CURRENT_SOURCE_DIR}/examples/preview/resources" NAMESPACE app)
    get_property(agentkit_preview_test_resources TARGET huxerui_agentkit_preview_tests PROPERTY HUXERUI_RESOURCE_PACKAGE)
    target_compile_definitions(huxerui_agentkit_preview_tests PRIVATE AGENTKIT_PREVIEW_TEST
            AGENTKIT_PREVIEW_CTEST="${CMAKE_CTEST_COMMAND}" AGENTKIT_PREVIEW_CMAKE="${CMAKE_COMMAND}"
            AGENTKIT_TEST_RESOURCE_PACKAGE="${agentkit_preview_test_resources}")
    # Native acceptance modes load resources from the executable's staged package.
    set_property(TARGET huxerui_agentkit_preview_tests APPEND PROPERTY HUXERUI_RESOURCE_PACKAGE_INPUTS
            "${HUXERUI_BUILTIN_RESOURCE_PACKAGE}")
    find_program(HUXERUI_AGENTKIT_TEST_BROWSER NAMES chrome chrome.exe chromium chromium-browser msedge msedge.exe
            HINTS "$ENV{ProgramFiles}/Google/Chrome/Application" "$ENV{ProgramW6432}/Google/Chrome/Application"
                  "$ENV{LOCALAPPDATA}/Google/Chrome/Application")
    if (HUXERUI_AGENTKIT_TEST_BROWSER)
        target_compile_definitions(huxerui_agentkit_preview_tests PRIVATE
                AGENTKIT_PREVIEW_BROWSER="${HUXERUI_AGENTKIT_TEST_BROWSER}")
    endif ()
    target_link_libraries(huxerui_agentkit_preview_tests PRIVATE HuxerUI::testing nlohmann_json::nlohmann_json)
    if (WIN32)
        target_link_libraries(huxerui_agentkit_preview_tests PRIVATE crypt32)
    endif ()
    include(examples/preview/cmake/LinkNavigation.cmake)
    agentkit_preview_link_navigation(huxerui_agentkit_preview_tests)
    include(examples/preview/cmake/MarkdownImages.cmake)
    agentkit_preview_link_markdown_images(huxerui_agentkit_preview_tests)
    agentkit_host_use_sqlite_session_store(huxerui_agentkit_preview_tests)
    agentkit_preview_link_archive(huxerui_agentkit_preview_tests)
    agentkit_host_link_terminal(huxerui_agentkit_preview_tests)
    include(examples/preview/cmake/CodeEditor.cmake)
    agentkit_preview_link_code_editor(huxerui_agentkit_preview_tests)
    add_test(NAME huxerui_agentkit_preview_tests COMMAND huxerui_agentkit_preview_tests)
    add_test(NAME huxerui_agentkit_localization_tests COMMAND huxerui_agentkit_preview_tests --localization)
    add_test(NAME huxerui_agentkit_code_turn_activity_tests COMMAND huxerui_agentkit_preview_tests --code-turn-activity)
    if (HUXERUI_AGENTKIT_TEST_BROWSER)
        add_test(NAME huxerui_agentkit_goal_completion_revision_tests COMMAND huxerui_agentkit_preview_tests --goal-completion-revision)
        add_test(NAME huxerui_agentkit_goal_completion_automatic_tests COMMAND huxerui_agentkit_preview_tests --goal-completion-automatic)
    endif ()
    if (WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_test(NAME huxerui_agentkit_release_test_failure_tests
                COMMAND huxerui_agentkit_preview_tests --release-verification-failure test-failure)
        add_test(NAME huxerui_agentkit_release_missing_screenshot_tests
                COMMAND huxerui_agentkit_preview_tests --release-verification-failure missing-screenshot)
    endif ()
    add_test(NAME huxerui_agentkit_navigation_tests COMMAND huxerui_agentkit_preview_tests --conversation-navigation)
    add_test(NAME huxerui_agentkit_review_approval_tests COMMAND huxerui_agentkit_preview_tests --review-approval)
    add_test(NAME huxerui_agentkit_long_conversation_tests COMMAND huxerui_agentkit_preview_tests --long-conversation)
    add_test(NAME huxerui_agentkit_composer_ime_tests COMMAND huxerui_agentkit_preview_tests --composer-ime)
    if (Python3_Interpreter_FOUND)
        add_test(NAME huxerui_agentkit_markdown_images_tests COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/markdown_image_server.py" $<TARGET_FILE:huxerui_agentkit_preview_tests>)
    endif ()
    add_test(NAME huxerui_agentkit_domain_review_tests COMMAND huxerui_agentkit_preview_tests --domain-review-windows)
    add_test(NAME huxerui_agentkit_code_revert_tests COMMAND huxerui_agentkit_preview_tests --code-revert)
    add_test(NAME huxerui_agentkit_code_revert_recovery_tests COMMAND huxerui_agentkit_preview_tests --code-revert-recovery)
    add_test(NAME huxerui_agentkit_code_failure_tests COMMAND huxerui_agentkit_preview_tests --code-failures)
    add_test(NAME huxerui_agentkit_code_patch_recovery_tests COMMAND huxerui_agentkit_preview_tests --code-patch-recovery)
    add_test(NAME huxerui_agentkit_code_editor_tests COMMAND huxerui_agentkit_preview_tests --code-editor)
    add_test(NAME huxerui_agentkit_code_editor_recovery_tests COMMAND ${CMAKE_COMMAND}
            -DPREVIEW_TEST=$<TARGET_FILE:huxerui_agentkit_preview_tests> -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/code_editor_recovery.cmake)
    add_test(NAME huxerui_agentkit_design_revert_tests COMMAND huxerui_agentkit_preview_tests --design-revert)
    add_test(NAME huxerui_agentkit_fork_review_tests COMMAND huxerui_agentkit_preview_tests --fork-review)
    add_test(NAME huxerui_agentkit_design_adoption_recovery_tests COMMAND huxerui_agentkit_preview_tests --design-adoption-recovery)
    add_test(NAME huxerui_agentkit_ppt_adoption_recovery_tests COMMAND huxerui_agentkit_preview_tests --ppt-adoption-recovery)
    add_test(NAME huxerui_agentkit_ppt_adoption_baseline_tests COMMAND huxerui_agentkit_preview_tests --ppt-adoption-baseline)
    add_test(NAME huxerui_agentkit_presentation_rework_tests COMMAND huxerui_agentkit_preview_tests --presentation-rework)
    add_test(NAME huxerui_agentkit_shared_report_tests COMMAND huxerui_agentkit_preview_tests --shared-reports)
    add_test(NAME huxerui_agentkit_presentation_failure_tests COMMAND huxerui_agentkit_preview_tests --presentation-failures)
    add_test(NAME huxerui_agentkit_side_chat_tests COMMAND huxerui_agentkit_preview_tests --side-chat)
    add_test(NAME huxerui_agentkit_artifact_side_chat_tests COMMAND huxerui_agentkit_preview_tests --artifact-side-chat)
    add_test(NAME huxerui_agentkit_report_chart_tests COMMAND huxerui_agentkit_preview_tests --report-chart)
    add_test(NAME huxerui_agentkit_release_skill_tests COMMAND huxerui_agentkit_preview_tests --release-skill)
    add_test(NAME huxerui_agentkit_run_settings_tests COMMAND huxerui_agentkit_preview_tests --run-settings)
    find_package(Git QUIET)
    if (Git_FOUND AND (WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Darwin" OR CMAKE_SYSTEM_NAME STREQUAL "Linux"))
        add_test(NAME huxerui_agentkit_worktree_tests COMMAND huxerui_agentkit_preview_tests --worktree)
        add_test(NAME huxerui_agentkit_code_candidate_tests COMMAND huxerui_agentkit_preview_tests --code-candidates)
        add_test(NAME huxerui_agentkit_code_adoption_recovery_tests COMMAND huxerui_agentkit_preview_tests --code-adoption-recovery)
        add_test(NAME huxerui_agentkit_partial_adoption_tests COMMAND huxerui_agentkit_preview_tests --code-adoption-partial)
        add_test(NAME huxerui_agentkit_candidate_recovery_tests COMMAND huxerui_agentkit_preview_tests --candidate-worktree-recovery)
        add_test(NAME huxerui_agentkit_approved_candidate_recovery_tests COMMAND huxerui_agentkit_preview_tests --approved-candidate-recovery)
    endif ()

    add_executable(huxerui_agentkit_presentation_tests
            tests/presentation_test.cpp
            examples/preview/src/presentation.cpp
    )
    target_include_directories(huxerui_agentkit_presentation_tests PRIVATE examples/preview/src)
    target_link_libraries(huxerui_agentkit_presentation_tests PRIVATE HuxerUI::AgentKit HuxerUI::testing)
    agentkit_preview_link_archive(huxerui_agentkit_presentation_tests)
    add_test(NAME huxerui_agentkit_presentation_tests COMMAND huxerui_agentkit_presentation_tests)
endif ()
