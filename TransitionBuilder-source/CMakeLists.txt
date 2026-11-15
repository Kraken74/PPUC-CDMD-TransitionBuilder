cmake_minimum_required(VERSION 3.20)
project(TransitionBuilder VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# On Windows the plugin carries its runtime inside, so nothing else has to be installed.
if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_library(plugin-transitionbuilder MODULE src/TransitionBuilder.cpp)
target_include_directories(plugin-transitionbuilder PRIVATE include src)
set_target_properties(plugin-transitionbuilder PROPERTIES PREFIX "")
if(WIN32)
  # The name plugin.cfg gives for windows.x64.
  set_target_properties(plugin-transitionbuilder PROPERTIES OUTPUT_NAME "plugin-transitionbuilder64")
endif()
if(MINGW)
  target_link_options(plugin-transitionbuilder PRIVATE -static -static-libgcc -static-libstdc++)
endif()
if(MSVC)
  target_compile_options(plugin-transitionbuilder PRIVATE /W4)
else()
  target_compile_options(plugin-transitionbuilder PRIVATE -Wall -Wextra)
endif()

# "cmake --install build --prefix dist" makes dist/TransitionBuilder: the folder to put into
# the editor's plugin folder.
install(TARGETS plugin-transitionbuilder LIBRARY DESTINATION TransitionBuilder
        RUNTIME DESTINATION TransitionBuilder)
install(FILES plugin.cfg README.md DESTINATION TransitionBuilder)

# Tests: a fake editor drives the plugin (not built for the Windows package).
option(TRANSITIONBUILDER_TESTS "Build the tests" OFF)
if(TRANSITIONBUILDER_TESTS)
  enable_testing()
  add_executable(transitionbuilder_tests tests/FakeEditorTest.cpp src/TransitionBuilder.cpp)
  target_include_directories(transitionbuilder_tests PRIVATE include src)
  if(NOT MSVC)
    target_compile_options(transitionbuilder_tests PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME transitionbuilder_tests COMMAND transitionbuilder_tests)
endif()
