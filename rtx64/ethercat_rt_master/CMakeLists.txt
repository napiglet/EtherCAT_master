set(RTX64_SDK_DIR "" CACHE PATH "RTX64 SDK root directory")
set(RTX64_INCLUDE_DIR "" CACHE PATH "RTX64 include directory")
set(RTX64_LIBRARY_DIR "" CACHE PATH "RTX64 library directory")
set(RTX64_LIBRARIES "" CACHE STRING "RTX64 libraries required by the RTSS target")

if(NOT WIN32)
  message(FATAL_ERROR "ethercat_rt_master is intended for Windows + RTX64.")
endif()

add_executable(ethercat_rt_master
  src/ethercat_rt_master.c
  include/ecat_rt_ipc.h
)

target_include_directories(ethercat_rt_master PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

if(RTX64_SDK_DIR)
  target_compile_definitions(ethercat_rt_master PRIVATE ECAT_RTX64_BUILD)
endif()

if(RTX64_INCLUDE_DIR)
  target_include_directories(ethercat_rt_master PRIVATE "${RTX64_INCLUDE_DIR}")
endif()

if(RTX64_LIBRARY_DIR)
  target_link_directories(ethercat_rt_master PRIVATE "${RTX64_LIBRARY_DIR}")
endif()

if(RTX64_LIBRARIES)
  target_link_libraries(ethercat_rt_master PRIVATE ${RTX64_LIBRARIES})
endif()

target_compile_options(ethercat_rt_master PRIVATE
  $<$<C_COMPILER_ID:MSVC>:/W3>
)

set_target_properties(ethercat_rt_master PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
  FOLDER "RTX64"
)
