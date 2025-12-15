add_executable(lmk
    lmk.c
)

target_link_libraries(lmk PRIVATE Freetype::Freetype X11::X11 ${XFT_LIBRARIES})
target_include_directories(lmk PRIVATE ${XFT_INCLUDE_DIRS})
