cmake_minimum_required(VERSION 3.16)
project(InkEngine VERSION 0.1.0 LANGUAGES CXX)

option(INK_BUILD_TESTS "Build InkEngine unit tests" ON)
option(INK_BUILD_TOOLS "Build InkEngine demo / tools" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

add_library(inkengine
  src/brush.cpp
  src/c_api.cpp
  src/correction.cpp
  src/editor.cpp
  src/eraser.cpp
  src/filters.cpp
  src/grain.cpp
  src/history.cpp
  src/page.cpp
  src/pressure_curve.cpp
  src/rasterizer.cpp
  src/selection.cpp
  src/serialization.cpp
  src/shape_recognizer.cpp
  src/spline.cpp
  src/stroke.cpp
  src/stroke_builder.cpp
  src/svg_export.cpp
  src/tessellator.cpp
)
add_library(InkEngine::inkengine ALIAS inkengine)
target_include_directories(inkengine PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
set_target_properties(inkengine PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(MSVC)
  target_compile_options(inkengine PRIVATE /W4)
else()
  target_compile_options(inkengine PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
endif()

if(INK_BUILD_TESTS)
  enable_testing()
  add_executable(ink_tests
    tests/test_main.cpp
    tests/test_filters.cpp
    tests/test_spline_geometry.cpp
    tests/test_builder.cpp
    tests/test_document.cpp
    tests/test_render.cpp
    tests/test_c_api.cpp
    tests/test_shapes_correction.cpp
    tools/synth_input.cpp
  )
  target_link_libraries(ink_tests PRIVATE inkengine)
  add_test(NAME ink_tests COMMAND ink_tests)
endif()

if(INK_BUILD_TOOLS)
  add_executable(ink_demo tools/ink_demo.cpp tools/synth_input.cpp tools/png_writer.cpp)
  target_link_libraries(ink_demo PRIVATE inkengine)
  add_executable(ink_bench tools/ink_bench.cpp tools/synth_input.cpp)
  target_link_libraries(ink_bench PRIVATE inkengine)
endif()

install(TARGETS inkengine EXPORT InkEngineTargets ARCHIVE DESTINATION lib LIBRARY DESTINATION lib)
install(DIRECTORY include/ DESTINATION include)
