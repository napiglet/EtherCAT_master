add_library(ethercat_dll SHARED
  src/ethercat_master.c
  src/ethercat_dll.rc
  include/ethercat_master.h
)

target_link_libraries(ethercat_dll PRIVATE soem ws2_32)

target_include_directories(ethercat_dll PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/common>
  $<INSTALL_INTERFACE:include>
)

target_compile_definitions(ethercat_dll PRIVATE
  ETHERCAT_MASTER_EXPORTS
  _CRT_SECURE_NO_WARNINGS
)

target_compile_options(ethercat_dll PRIVATE
  $<$<C_COMPILER_ID:MSVC>:/W3>
  $<$<C_COMPILER_ID:GNU>:-Wall -Wextra -Wno-unused-parameter>
)

set_target_properties(ethercat_dll PROPERTIES
  OUTPUT_NAME "ethercat_dll"
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

add_executable(ethercat_dll_test test/ethercat_dll_test.c)
target_link_libraries(ethercat_dll_test PRIVATE ethercat_dll)

set_target_properties(ethercat_dll_test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

set(ETHERCAT_SDK_DIR "${CMAKE_BINARY_DIR}/ethercat_sdk")

add_custom_command(TARGET ethercat_dll POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory
          "${ETHERCAT_SDK_DIR}/include"
          "${ETHERCAT_SDK_DIR}/lib"
          "${ETHERCAT_SDK_DIR}/bin"
          "${ETHERCAT_SDK_DIR}/examples"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_CURRENT_SOURCE_DIR}/include/ethercat_master.h"
          "${ETHERCAT_SDK_DIR}/include/ethercat_master.h"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_SOURCE_DIR}/common/ecat_protocol.h"
          "${ETHERCAT_SDK_DIR}/include/ecat_protocol.h"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "$<TARGET_FILE:ethercat_dll>"
          "${ETHERCAT_SDK_DIR}/bin/$<TARGET_FILE_NAME:ethercat_dll>"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "$<TARGET_LINKER_FILE:ethercat_dll>"
          "${ETHERCAT_SDK_DIR}/lib/$<TARGET_LINKER_FILE_NAME:ethercat_dll>"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_CURRENT_SOURCE_DIR}/test/ethercat_dll_test.c"
          "${ETHERCAT_SDK_DIR}/examples/ethercat_dll_test.c"
)

add_custom_command(TARGET ethercat_dll_test POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "$<TARGET_FILE:ethercat_dll_test>"
          "${ETHERCAT_SDK_DIR}/bin/$<TARGET_FILE_NAME:ethercat_dll_test>"
)

install(TARGETS ethercat_dll ethercat_dll_test
  RUNTIME DESTINATION bin
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
)

install(FILES include/ethercat_master.h DESTINATION include)
