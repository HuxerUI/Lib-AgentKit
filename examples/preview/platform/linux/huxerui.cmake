function(huxerui_configure_linux_project_package target_name install_component)
    if (EXISTS "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/package/${target_name}.svg"
            AND EXISTS "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/package/${target_name}.desktop")
        add_custom_command(TARGET ${target_name} POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E make_directory
                        "$<TARGET_FILE_DIR:${target_name}>/share/icons/hicolor/scalable/apps"
                        "$<TARGET_FILE_DIR:${target_name}>/share/applications"
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/package/${target_name}.svg"
                        "$<TARGET_FILE_DIR:${target_name}>/share/icons/hicolor/scalable/apps/${target_name}.svg"
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/package/${target_name}.desktop"
                        "$<TARGET_FILE_DIR:${target_name}>/share/applications/${target_name}.desktop"
                VERBATIM
        )
    endif ()

    if (NOT HUXERUI_PACKAGE)
        return()
    endif ()

    install(TARGETS ${target_name}
            RUNTIME DESTINATION usr/bin
            COMPONENT "${install_component}"
    )
    _huxerui_install_runtime_dependencies(${target_name} "${install_component}"
            usr "bin/$<TARGET_FILE_NAME:${target_name}>"
    )
    get_target_property(HUXERUI_LINUX_APP_RESOURCES
            ${target_name}
            HUXERUI_RESOURCE_PACKAGE
    )
    if (HUXERUI_LINUX_APP_RESOURCES
            AND NOT HUXERUI_LINUX_APP_RESOURCES MATCHES "-NOTFOUND$")
        install(DIRECTORY "${HUXERUI_LINUX_APP_RESOURCES}/"
                DESTINATION "usr/bin/${target_name}.resources"
                COMPONENT "${install_component}"
        )
    endif ()
    install(PROGRAMS "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/package/AppRun"
            DESTINATION .
            COMPONENT "${install_component}"
    )
    install(FILES
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/package/ganwork.desktop"
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/package/ganwork.svg"
            DESTINATION .
            COMPONENT "${install_component}"
    )
    install(FILES
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/package/ganwork.desktop"
            DESTINATION usr/share/applications
            COMPONENT "${install_component}"
    )
    install(FILES
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/package/ganwork.svg"
            DESTINATION usr/share/icons/hicolor/scalable/apps
            COMPONENT "${install_component}"
    )
endfunction()
