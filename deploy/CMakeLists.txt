# SPDX-FileCopyrightText: (C) 2024 Chris Rizzitello <sithlord48@gmail.com>
# SPDX-License-Identifier: MIT

# Copy License with txt ext for picky package creation tools
file(COPY_FILE
  ${CMAKE_SOURCE_DIR}/LICENSE
  ${CMAKE_CURRENT_BINARY_DIR}/LICENSE.txt
  ONLY_IF_DIFFERENT
)

# Generic Package Items
set(CPACK_STRIP_FILES TRUE)
set(CPACK_PACKAGE_NAME ${CMAKE_PROJECT_PROPER_NAME})
set(CPACK_PACKAGE_CONTACT ${CMAKE_PROJECT_CONTACT})
set(CPACK_PACKAGE_DESCRIPTION ${CMAKE_PROJECT_DESCRIPTION})
set(CPACK_PACKAGE_VENDOR ${CMAKE_PROJECT_VENDOR})
set(CPACK_RESOURCE_FILE_LICENSE ${CMAKE_CURRENT_BINARY_DIR}/LICENSE.txt)

set(CPACK_PACKAGE_VERSION ${CMAKE_PROJECT_VERSION})

#Prevent this override from being written in the package
if(NOT PACKAGE_VERSION_LABEL)
    set (PACKAGE_VERSION_LABEL "${CPACK_PACKAGE_VERSION}")
endif()

include(windows/deploy.cmake)

# Keep package artifacts aligned with the branded application name.
set(CPACK_PACKAGE_FILE_NAME "${CMAKE_PROJECT_PROPER_NAME}-${PACKAGE_VERSION_LABEL}-${OS_STRING}")
message(STATUS "Package Basename: ${CPACK_PACKAGE_FILE_NAME}")

include(CPack)
