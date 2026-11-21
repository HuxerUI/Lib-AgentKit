function(agentkit_preview_link_archive target)
    if (POLICY CMP0168)
        set(CMAKE_POLICY_DEFAULT_CMP0168 NEW)
    endif ()
    get_filename_component(archive_sibling
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../../../../Lib-Archive" ABSOLUTE)
    set(HUXERUI_AGENTKIT_ARCHIVE_SOURCE_DIR "" CACHE PATH "Local Lib-Archive source for GanWork")
    if (HUXERUI_AGENTKIT_ARCHIVE_SOURCE_DIR)
        huxerui_use_library(${target} TARGET HuxerUI::Archive PATH "${HUXERUI_AGENTKIT_ARCHIVE_SOURCE_DIR}")
    elseif (EXISTS "${archive_sibling}/CMakeLists.txt")
        huxerui_use_library(${target} TARGET HuxerUI::Archive PATH "${archive_sibling}")
    else ()
        huxerui_use_library(${target}
                TARGET HuxerUI::Archive
                URL "https://github.com/HuxerUI/Lib-Archive.git"
                COMMIT "1ab08f035667cee4a704bfe097181264c7b715fb"
        )
    endif ()
endfunction()
