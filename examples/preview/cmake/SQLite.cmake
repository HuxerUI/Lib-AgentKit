function(agentkit_preview_link_sqlite target)
    get_filename_component(sqlite_sibling
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../../../../Lib-SQLite" ABSOLUTE)
    set(HUXERUI_AGENTKIT_SQLITE_SOURCE_DIR "" CACHE PATH "Local Lib-SQLite source for AgentKit Preview")
    if (HUXERUI_AGENTKIT_SQLITE_SOURCE_DIR)
        huxerui_use_library(${target} TARGET HuxerUI::SQLite PATH "${HUXERUI_AGENTKIT_SQLITE_SOURCE_DIR}")
    elseif (EXISTS "${sqlite_sibling}/CMakeLists.txt")
        huxerui_use_library(${target} TARGET HuxerUI::SQLite PATH "${sqlite_sibling}")
    else ()
        huxerui_use_library(${target}
                TARGET HuxerUI::SQLite
                URL "https://github.com/HuxerUI/Lib-SQLite.git"
                COMMIT "882665a9772cc973b1cbfb7f491913703da1d636"
        )
    endif ()
endfunction()
