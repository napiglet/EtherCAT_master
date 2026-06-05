add_executable(lms_master lms_master.c)
target_link_libraries(lms_master soem)

target_compile_options(lms_master PRIVATE
  $<$<C_COMPILER_ID:MSVC>:
    /D _CRT_SECURE_NO_WARNINGS
    /W3
  >
  $<$<C_COMPILER_ID:GNU>:
    -Wall
    -Wextra
    -Wno-unused-parameter
  >
)

install(TARGETS lms_master DESTINATION bin)
