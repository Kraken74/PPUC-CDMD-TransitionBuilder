cmake_minimum_required(VERSION 3.20)
project(TransitionFX VERSION 0.2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# On Windows the plugin carries its runtime inside, so nothing else has to be installed.
if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_library(plugin-transitionfx MODULE src/TransitionFX.cpp)
target_include_directories(plugin-transitionfx PRIVATE include src)
set_target_properties(plugin-transitionfx PROPERTIES PREFIX "")
if(WIN32)
  # The name plugin.cfg gives for windows.x64.
  set_target_properties(plugin-transitionfx PROPERTIES OUTPUT_NAME "plugin-transitionfx64")
endif()
if(APPLE)
  # The name plugin.cfg gives for macos (a CMake module is a .so there otherwise).
  set_target_properties(plugin-transitionfx PROPERTIES SUFFIX ".dylib")
endif()
if(MINGW)
  target_link_options(plugin-transitionfx PRIVATE -static -static-libgcc -static-libstdc++)
endif()
if(MSVC)
  target_compile_options(plugin-transitionfx PRIVATE /W4)
else()
  target_compile_options(plugin-transitionfx PRIVATE -Wall -Wextra)
endif()

# "cmake --install build --prefix dist" makes dist/TransitionFX: the folder to put into
# the editor's plugin folder.
install(TARGETS plugin-transitionfx LIBRARY DESTINATION TransitionFX
        RUNTIME DESTINATION TransitionFX)
install(FILES plugin.cfg README.md DESTINATION TransitionFX)

# Tests: a fake editor drives the plugin (not built for the Windows package).
option(TRANSITIONFX_TESTS "Build the tests" OFF)
if(TRANSITIONFX_TESTS)
  enable_testing()
  add_executable(transitionfx_tests tests/FakeEditorTest.cpp src/TransitionFX.cpp)
  target_include_directories(transitionfx_tests PRIVATE include src)
  if(NOT MSVC)
    target_compile_options(transitionfx_tests PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME transitionfx_tests COMMAND transitionfx_tests)
endif()
