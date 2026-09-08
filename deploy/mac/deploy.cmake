# SPDX-FileCopyrightText: (C) 2024 Chris Rizzitello <sithlord48@gmail.com>
# SPDX-License-Identifier: MIT

# HACK This is set when the files is included so its the real path
# calling CMAKE_CURRENT_LIST_DIR after include would return the wrong scope var
set(MY_DIR ${CMAKE_CURRENT_LIST_DIR})
set(OSX_BUNDLE ${BUILD_OSX_BUNDLE})

set(OS_STRING "macos-${BUILD_ARCHITECTURE}")

if (OSX_BUNDLE)
  set(_codesign_identity "-")
  set(_deployqt_runtime_args)
  set(_codesign_runtime_args)
  if (APPLE_CODESIGN_DEV)
    set(_codesign_identity "${APPLE_CODESIGN_DEV}")
    set(_deployqt_runtime_args -hardened-runtime)
    set(_codesign_runtime_args --options runtime)
  endif()
  foreach(_bundle_name "ZeroFlow Server" "ZeroFlow Client")
    set(_deployqt_executable_arg)
    if(_bundle_name STREQUAL "ZeroFlow Server")
      set(_deployqt_executable_arg
        "-executable=\${CMAKE_INSTALL_PREFIX}/${_bundle_name}.app/Contents/MacOS/zeroflow-core"
      )
    endif()
    install(CODE "execute_process(COMMAND
      ${DEPLOYQT}
      \"\${CMAKE_INSTALL_PREFIX}/${_bundle_name}.app\"
      ${_deployqt_executable_arg}
      ${_deployqt_runtime_args} -timestamp \"-codesign=${_codesign_identity}\"
      COMMAND_ERROR_IS_FATAL ANY
    )")
    # Keep all nested Homebrew frameworks under one signature. Hardened runtime
    # is valid only with a Developer ID because ad-hoc signatures have no Team ID.
    install(CODE "execute_process(COMMAND
      codesign --force --deep --sign \"${_codesign_identity}\" ${_codesign_runtime_args}
      \"\${CMAKE_INSTALL_PREFIX}/${_bundle_name}.app\"
      COMMAND_ERROR_IS_FATAL ANY
    )")
  endforeach()
  set(CPACK_PACKAGE_ICON "${MY_DIR}/dmg-volume.icns")
  set(CPACK_DMG_BACKGROUND_IMAGE "${MY_DIR}/dmg-background.tiff")
  set(CPACK_DMG_DS_STORE_SETUP_SCRIPT "${MY_DIR}/generate_ds_store.applescript")
  set(CPACK_DMG_VOLUME_NAME "${CMAKE_PROJECT_PROPER_NAME}")
  set(CPACK_DMG_SLA_USE_RESOURCE_FILE_LICENSE ON)
  set(CPACK_GENERATOR "DragNDrop")
endif()
