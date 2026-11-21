function(agentkit_host_link_terminal target)
    if (POLICY CMP0168)
        set(CMAKE_POLICY_DEFAULT_CMP0168 NEW)
    endif ()
    get_filename_component(terminal_sibling
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../../Lib-Terminal" ABSOLUTE)
    if (EXISTS "${terminal_sibling}/CMakeLists.txt")
        huxerui_use_library(${target} TARGET HuxerUI::Terminal PATH "${terminal_sibling}")
    else ()
        huxerui_use_library(${target}
                TARGET HuxerUI::Terminal
                URL "https://github.com/HuxerUI/Lib-Terminal.git"
                COMMIT "cfb3d0f23730724b90e666583e1cb0236bfba654")
    endif ()
endfunction()
