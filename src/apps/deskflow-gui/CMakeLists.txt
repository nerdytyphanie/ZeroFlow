# SPDX-FileCopyrightText: (C) 2024 - 2025 Chris Rizzitello <sithlord48@gmail.com>
# SPDX-FileCopyrightText: (C) 2024 Symless Ltd
# SPDX-License-Identifier: MIT

set(CMAKE_INCLUDE_CURRENT_DIR ON)

function(add_zeroflow_app target output_name role display_name bundle_id)
  unset(platform_extra)

  if(WIN32)
    set(EXE_DESCRIPTION "${display_name}")
    set(EXE_ICON "IDI_ICON1 ICON DISCARDABLE \"${CMAKE_SOURCE_DIR}/src/apps/res/deskflow.ico\" ")
    configure_file(${CMAKE_SOURCE_DIR}/src/apps/res/windows.rc.in ${target}.rc)
    set(platform_extra ${target}.rc)
  elseif(BUILD_OSX_BUNDLE)
    set(BUNDLE_EXECUTABLE_NAME "${output_name}")
    set(BUNDLE_BUNDLE_NAME "${display_name}")
    set(BUNDLE_DISPLAY_NAME "${display_name}")
    set(BUNDLE_GUI_IDENTIFIER "${bundle_id}")
    set(BUNDLE_ICON_FILE Deskflow.icns)
    set(BUNDLE_INFO_STRING "${CMAKE_PROJECT_DESCRIPTION}")
    set(BUNDLE_COPYRIGHT "${CMAKE_PROJECT_COPYRIGHT}")

    configure_file(
      ${CMAKE_CURRENT_SOURCE_DIR}/../res/deskflow.plist.in
      ${CMAKE_CURRENT_BINARY_DIR}/${target}.plist
      @ONLY
    )

    set(platform_extra ../res/Deskflow.icns)
    set_source_files_properties(${platform_extra} PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
  endif()

  add_executable(${target} WIN32 MACOSX_BUNDLE
    ${platform_extra}
    ../res/deskflow.qrc
    deskflow-gui.cpp
  )

  set_target_properties(${target} PROPERTIES OUTPUT_NAME "${output_name}")
  target_compile_definitions(${target} PRIVATE ZEROFLOW_${role}_APP)
  target_link_libraries(
    ${target}
    gui
    common
    Qt6::Core
    Qt6::Widgets
    Qt6::Network
  )

  if(APPLE AND BUILD_OSX_BUNDLE)
    install(TARGETS ${target} BUNDLE DESTINATION .)
  else()
    install(
      TARGETS ${target}
      RUNTIME_DEPENDENCY_SET guiDeps
      RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
  endif()

  if(WIN32)
    set_target_properties(${target} PROPERTIES LINK_FLAGS "/NODEFAULTLIB:LIBCMT")

    set(QT_DEPENDS_DIR ${CMAKE_BINARY_DIR}/qt-depends)
    add_custom_command(
      TARGET ${target} POST_BUILD
      COMMAND ${DEPLOYQT}
        --no-compiler-runtime
        --no-system-d3d-compiler
        --no-opengl-sw
        --no-quick-import
        --dir "${QT_DEPENDS_DIR}"
        --plugindir "${QT_DEPENDS_DIR}/plugins"
        $<TARGET_FILE:${target}>
      COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
        ${QT_DEPENDS_DIR}
        $<TARGET_FILE_DIR:${target}>
    )
  elseif(APPLE)
    if(BUILD_OSX_BUNDLE)
      set_target_properties(${target} PROPERTIES
        INSTALL_RPATH "@loader_path/../Libraries;@loader_path/../Frameworks"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/${target}.plist"
        XCODE_ATTRIBUTE_ENABLE_HARDENED_RUNTIME YES
      )
      if(APPLE_CODESIGN_DEV)
        configure_mac_codesign(${target})
      endif()
      add_dependencies(${target} zeroflow-core)
      add_custom_command(
        TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
          $<TARGET_FILE:zeroflow-core>
          $<TARGET_BUNDLE_CONTENT_DIR:${target}>/MacOS/zeroflow-core
      )
    else()
      set_target_properties(${target} PROPERTIES MACOSX_BUNDLE FALSE)
    endif()
  else()
    generate_app_man(${target} "${display_name}")
  endif()
endfunction()

if(APPLE)
  add_zeroflow_app(zeroflow-server "ZeroFlow Server" SERVER "ZeroFlow Server" "${CMAKE_PROJECT_REV_FQDN}.server")
else()
  add_zeroflow_app(zeroflow-server zeroflow-server SERVER "ZeroFlow Server" "${CMAKE_PROJECT_REV_FQDN}.server")
  if(WIN32)
    add_zeroflow_app(zeroflow-client zeroflow-client CLIENT "ZeroFlow Client" "${CMAKE_PROJECT_REV_FQDN}.client")
  endif()
endif()

if(WIN32)
  install(RUNTIME_DEPENDENCY_SET guiDeps
    ${WIN32_RUNTIME_DEPENDENCY_SET_ARGS}
    PRE_EXCLUDE_REGEXES ${WIN32_PRE_EXCLUDE_REGEXES}
    POST_EXCLUDE_REGEXES ${WIN32_POST_EXCLUDE_REGEXES}
    RUNTIME DESTINATION ${CMAKE_INSTALL_LIBDIR}
  )

  install(
    DIRECTORY ${CMAKE_BINARY_DIR}/qt-depends/
    DESTINATION ${CMAKE_INSTALL_LIBDIR}
    PATTERN "dx*.dll" EXCLUDE
    PATTERN "icu*.dll" EXCLUDE
  )
endif()
