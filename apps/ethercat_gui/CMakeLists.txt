add_executable(ethercat_gui WIN32
  ethercat_gui.c
  ethercat_gui.rc
  resource.h
)

target_link_libraries(ethercat_gui PRIVATE
  ethercat_dll
  user32
  gdi32
  comctl32
  comdlg32
)

target_compile_definitions(ethercat_gui PRIVATE
  _CRT_SECURE_NO_WARNINGS
)

target_compile_options(ethercat_gui PRIVATE
  $<$<C_COMPILER_ID:MSVC>:
    /W3
  >
  $<$<C_COMPILER_ID:GNU>:
    -Wall
    -Wextra
    -Wno-unused-parameter
  >
)

add_custom_command(TARGET ethercat_gui POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "$<TARGET_FILE:ethercat_dll>"
          "$<TARGET_FILE_DIR:ethercat_gui>/$<TARGET_FILE_NAME:ethercat_dll>"
)

install(TARGETS ethercat_gui DESTINATION bin)
