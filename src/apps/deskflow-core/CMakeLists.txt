# SPDX-FileCopyrightText: (C) 2024 - 2025 Chris Rizzitello <sithlord48@gmail.com>
# SPDX-FileCopyrightText: (C) 2012 - 2024 Symless Ltd
# SPDX-FileCopyrightText: (C) 2009 - 2012 Nick Bolton
# SPDX-License-Identifier: MIT

set(target zeroflow-core)

set(core_sources
  deskflow-core.cpp
)

add_executable(${target} ${core_sources})

if(WIN32)
  # Generate rc file
  set(EXE_DESCRIPTION "ZeroFlow")
  set(EXE_ICON "IDI_ZEROFLOW ICON DISCARDABLE \"${CMAKE_SOURCE_DIR}/src/apps/res/zeroflow-embedded.ico\"")
  configure_file(${CMAKE_SOURCE_DIR}/src/apps/res/windows.rc.in ${target}.rc)

  target_sources(${target} PRIVATE
    deskflow-core.exe.manifest
    ${CMAKE_SOURCE_DIR}/src/apps/res/zeroflow-embedded.ico
    ${CMAKE_CURRENT_BINARY_DIR}/${target}.rc
  )
endif()

target_link_libraries(
  ${target}
  arch
  base
  client
  io
  mt
  net
  platform
  server
  app
  Qt6::Core
  ${libs})


# A subsystem without a console prevents flashes even when launched directly.
set_target_properties(${target} PROPERTIES WIN32_EXECUTABLE TRUE OUTPUT_NAME "ZeroFlow")
target_link_options(${target} PRIVATE /ENTRY:mainCRTStartup)
install(TARGETS ${target} RUNTIME DESTINATION .)

option(BUILD_HEADLESS_TESTS "Build separate headless regression harnesses" OFF)
if(BUILD_HEADLESS_TESTS)
  add_executable(clipboard-limit-test ${CMAKE_SOURCE_DIR}/tests/clipboard_limit.cpp)
  get_target_property(core_test_libraries ${target} LINK_LIBRARIES)
  target_link_libraries(clipboard-limit-test PRIVATE ${core_test_libraries})
endif()
