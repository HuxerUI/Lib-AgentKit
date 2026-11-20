function(agentkit_preview_link_markdown_images target)
    cmake_policy(PUSH)
    if (POLICY CMP0168)
        cmake_policy(SET CMP0168 NEW)
    endif ()
    include(FetchContent)
    FetchContent_Declare(agentkit_preview_stb_image
            URL https://raw.githubusercontent.com/nothings/stb/9600522de53973336fa033a289858a4354cbb04c/stb_image.h
            URL_HASH SHA256=deb4f18c9ca21b751a1323019031c94c0bf5ca1d2929ef5913cc56f00aab6e91
            DOWNLOAD_NO_EXTRACT TRUE
    )
    FetchContent_MakeAvailable(agentkit_preview_stb_image)
    cmake_policy(POP)
    target_include_directories(${target} SYSTEM PRIVATE "${agentkit_preview_stb_image_SOURCE_DIR}")
endfunction()
