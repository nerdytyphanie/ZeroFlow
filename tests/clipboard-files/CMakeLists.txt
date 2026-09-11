cmake_minimum_required(VERSION 3.24)
project(ZeroFlowClipboardFilesTests LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
find_package(Qt6 REQUIRED COMPONENTS Core Network)
find_package(OpenSSL REQUIRED)
add_executable(clipboard-files-tests main.cpp
  ../../src/lib/platform/FileClipboardTransfer.cpp
  ../../src/lib/platform/MSWindowsClipboardFilesConverter.cpp)
target_include_directories(clipboard-files-tests PRIVATE ../../src/lib)
target_link_libraries(clipboard-files-tests PRIVATE shell32 ole32 userenv wtsapi32 advapi32 Qt6::Core Qt6::Network OpenSSL::Crypto)
enable_testing()
add_test(NAME clipboard-files COMMAND clipboard-files-tests)
