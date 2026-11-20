cmake_minimum_required(VERSION 3.20)

# CMake initializes Apple deployment defaults during the first project() call.
# Explicit settings and toolchains retain ownership of their target platform.
if (CMAKE_HOST_APPLE AND (NOT CMAKE_SYSTEM_NAME OR CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        AND NOT CMAKE_TOOLCHAIN_FILE AND "$ENV{CMAKE_TOOLCHAIN_FILE}" STREQUAL ""
        AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET AND "$ENV{MACOSX_DEPLOYMENT_TARGET}" STREQUAL "")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum macOS deployment target")
endif ()

project(example_agent_kit VERSION 0.1.0 LANGUAGES NONE)

include("${CMAKE_CURRENT_SOURCE_DIR}/HuxerUIProject.cmake")
if (HUXERUI_PROJECT_PLAN_ONLY)
    return()
endif ()

file(GLOB_RECURSE APP_SOURCE_FILES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cxx"
)
if (EMSCRIPTEN)
    list(REMOVE_ITEM APP_SOURCE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/src/sqlite_store.cpp")
else ()
    list(REMOVE_ITEM APP_SOURCE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/src/browser_store.cpp")
endif ()
if (NOT HUXERUI_LIBRARY_GRAPH_ONLY)
    if (WIN32)
        list(APPEND APP_SOURCE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/platform/windows/main.cpp")
    elseif (APPLE AND NOT IOS)
        list(APPEND APP_SOURCE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/platform/macos/main.cpp")
    elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux")
        list(APPEND APP_SOURCE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/platform/linux/main.cpp")
    endif ()
endif ()

huxerui_add_app(example_agent_kit
        SOURCES
            ${APP_SOURCE_FILES}
        RESOURCES
            resources
        RESOURCE_NAMESPACE
            app
        ${HUXERUI_APP_RESOURCE_OUTPUT_ARGUMENTS}
        BUNDLE_NAME
            "AgentKit Preview"
        BUNDLE_IDENTIFIER
            "org.huxerui.lib.agentkit.preview"
)

huxerui_configure_project_app(example_agent_kit)

huxerui_use_library(example_agent_kit
        TARGET HuxerUI::AgentKit
        PATH "${CMAKE_CURRENT_SOURCE_DIR}/../.."
)
if (NOT HUXERUI_LIBRARY_GRAPH_ONLY)
    target_link_libraries(example_agent_kit PRIVATE nlohmann_json::nlohmann_json)
    if (WIN32)
        target_link_libraries(example_agent_kit PRIVATE crypt32)
    endif ()
    include(cmake/LinkNavigation.cmake)
    agentkit_preview_link_navigation(example_agent_kit)
    include(cmake/MarkdownImages.cmake)
    agentkit_preview_link_markdown_images(example_agent_kit)
    if (MSVC)
        target_compile_options(example_agent_kit PRIVATE /bigobj)
    endif ()
    include(cmake/Skills.cmake)
    agentkit_preview_embed_skills(example_agent_kit)
endif ()
if (NOT EMSCRIPTEN)
    include(cmake/SQLite.cmake)
    agentkit_preview_link_sqlite(example_agent_kit)
endif ()
include(cmake/Archive.cmake)
agentkit_preview_link_archive(example_agent_kit)
include(cmake/Terminal.cmake)
agentkit_preview_link_terminal(example_agent_kit)
include(cmake/CodeEditor.cmake)
agentkit_preview_link_code_editor(example_agent_kit)
