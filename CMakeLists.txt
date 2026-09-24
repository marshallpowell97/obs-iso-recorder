cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" ON)
option(ENABLE_QT "Use Qt functionality" OFF)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Widgets Core)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/plugin-main.c
    src/iso-filter.c
    src/iso-output.c
    src/iso-output.h
    src/compat/buffered-file-serializer.h
    src/timecode.c
    src/timecode.h
    src/mux/mp4-mux.c
    src/mux/mp4-mux.h
    src/mux/mp4-mux-internal.h
    src/mux/rtmp-hevc.c
    src/mux/rtmp-hevc.h
    src/mux/rtmp-av1.c
    src/mux/rtmp-av1.h
    src/mux/utils.h
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE src src/mux)

option(ENABLE_TESTS "Build unit tests" OFF)
if(ENABLE_TESTS)
  add_executable(timecode-test tests/timecode-test.c src/timecode.c)
  target_include_directories(timecode-test PRIVATE src)
  target_link_libraries(timecode-test PRIVATE OBS::libobs)
endif()

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
