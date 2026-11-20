function(agentkit_preview_link_code_editor target)
    get_filename_component(editor_sibling
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../../../../Lib-CodeEditor" ABSOLUTE)
    if (EXISTS "${editor_sibling}/CMakeLists.txt")
        huxerui_use_library(${target} TARGET HuxerUI::CodeEditor PATH "${editor_sibling}")
    else ()
        huxerui_use_library(${target}
                TARGET HuxerUI::CodeEditor
                URL "https://github.com/HuxerUI/Lib-CodeEditor.git"
                COMMIT "4e67fd0c33a927f49727f22b90b02862ddd7d3ab")
    endif ()
    if (WIN32 AND NOT HUXERUI_LIBRARY_GRAPH_ONLY)
        target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    endif ()
endfunction()
