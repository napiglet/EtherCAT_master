add_executable(ethercat_rt_core
  src/ethercat_rt_core.c
  "${CMAKE_SOURCE_DIR}/common/ecat_protocol.h"
)

target_include_directories(ethercat_rt_core PRIVATE
  "${CMAKE_SOURCE_DIR}/common"
)

if(WIN32)
  target_link_libraries(ethercat_rt_core PRIVATE ws2_32)
endif()

target_compile_options(ethercat_rt_core PRIVATE
  $<$<C_COMPILER_ID:MSVC>:/W3>
  $<$<C_COMPILER_ID:GNU>:-Wall -Wextra -Wno-unused-parameter>
)

set_target_properties(ethercat_rt_core PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

option(ECAT_RT_CORE_ENABLE_IGH "Build IgH/Xenomai EtherCAT monitor tools" OFF)
set(ECAT_XENOMAI_CONFIG "/usr/xenomai/bin/xeno-config" CACHE FILEPATH
  "Path to Xenomai xeno-config")

if(UNIX AND NOT APPLE AND ECAT_RT_CORE_ENABLE_IGH)
  find_path(ECAT_IGH_INCLUDE_DIR ecrt.h
    PATHS /usr/include /usr/local/include)
  find_library(ECAT_IGH_LIBRARY ethercat
    PATHS /usr/lib /usr/local/lib /usr/lib/x86_64-linux-gnu)

  if(NOT ECAT_IGH_INCLUDE_DIR OR NOT ECAT_IGH_LIBRARY)
    message(FATAL_ERROR
      "IgH EtherCAT user library/header not found. Expected ecrt.h and libethercat.")
  endif()

  add_executable(ethercat_igh_lts_monitor
    src/igh_lts_monitor.c
    src/motion_cia402.c
    include/motion_cia402.h
  )

  add_executable(ethercat_igh_backend_server
    src/igh_lts_backend_server.c
    src/motion_cia402.c
    include/motion_cia402.h
    "${CMAKE_SOURCE_DIR}/common/ecat_protocol.h"
  )

  foreach(ECAT_IGH_TARGET
      ethercat_igh_lts_monitor
      ethercat_igh_backend_server)
    target_include_directories(${ECAT_IGH_TARGET} PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/include"
      "${CMAKE_SOURCE_DIR}/common"
      "${ECAT_IGH_INCLUDE_DIR}"
    )

    target_link_libraries(${ECAT_IGH_TARGET} PRIVATE
      "${ECAT_IGH_LIBRARY}"
      pthread
      rt
    )
  endforeach()

  if(EXISTS "${ECAT_XENOMAI_CONFIG}")
    execute_process(
      COMMAND "${ECAT_XENOMAI_CONFIG}" --posix --cflags
      OUTPUT_VARIABLE ECAT_XENOMAI_CFLAGS
      OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    execute_process(
      COMMAND "${ECAT_XENOMAI_CONFIG}" --posix --ldflags
      OUTPUT_VARIABLE ECAT_XENOMAI_LDFLAGS
      OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    separate_arguments(ECAT_XENOMAI_CFLAGS UNIX_COMMAND "${ECAT_XENOMAI_CFLAGS}")
    separate_arguments(ECAT_XENOMAI_LDFLAGS UNIX_COMMAND "${ECAT_XENOMAI_LDFLAGS}")
    foreach(ECAT_IGH_TARGET
        ethercat_igh_lts_monitor
        ethercat_igh_backend_server)
      target_compile_options(${ECAT_IGH_TARGET} PRIVATE
        ${ECAT_XENOMAI_CFLAGS}
      )
      target_link_options(${ECAT_IGH_TARGET} PRIVATE
        ${ECAT_XENOMAI_LDFLAGS}
      )
      target_compile_definitions(${ECAT_IGH_TARGET} PRIVATE
        ECAT_USE_XENOMAI_POSIX=1
      )
    endforeach()
  else()
    message(WARNING
      "Xenomai xeno-config not found. Building IgH tools with Linux POSIX timing only.")
  endif()

  foreach(ECAT_IGH_TARGET
      ethercat_igh_lts_monitor
      ethercat_igh_backend_server)
    target_compile_options(${ECAT_IGH_TARGET} PRIVATE
      -Wall -Wextra -Wno-unused-parameter
    )

    set_target_properties(${ECAT_IGH_TARGET} PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
  endforeach()
endif()
