cmake_minimum_required(VERSION 3.24)
project(AC8GMeter VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(FT_BUILD_TESTS "Build unit tests" ON)
option(FT_BUILD_MACOS_PROTO "Build the SDL3 + Metal standalone prototype (macOS only)" ${APPLE})
option(FT_BUILD_WIN_PROTO "Build the Win32 + Direct3D 12 standalone prototype (Windows only)" ${WIN32})

# Sources are UTF-8; without this MSVC reads them in the system code page (e.g. 936).
if(MSVC)
  add_compile_options(/utf-8)
endif()

include(cmake/Deps.cmake)

# Warning flags for first-party targets only (never applied to third-party code).
function(ft_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive-)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
  endif()
endfunction()

# Each layer is a static library. Sources are globbed so new files are picked up
# automatically; re-run CMake (or rely on CONFIGURE_DEPENDS) after adding files.
#   ft_core    : pure C++20, zero dependencies (math, filters, metrics, mission stats, pipeline)
#   ft_sources : telemetry sources (simulated flight model, CSV replay, CSV recorder)
#   ft_config  : settings model + JSON persistence
#   ft_storage : mission archive (finished missions saved to / loaded from disk on a worker thread)
#   ft_ui      : Dear ImGui overlay / menus. No platform or graphics-API code.
foreach(lib core sources config storage ui)
  file(GLOB_RECURSE _srcs CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/${lib}/*.cpp ${CMAKE_SOURCE_DIR}/src/${lib}/*.h)
  add_library(ft_${lib} STATIC ${_srcs})
  target_include_directories(ft_${lib} PUBLIC ${CMAKE_SOURCE_DIR}/src)
  set_target_properties(ft_${lib} PROPERTIES LINKER_LANGUAGE CXX)
  ft_warnings(ft_${lib})
endforeach()
target_link_libraries(ft_sources PUBLIC ft_core)
target_link_libraries(ft_config  PUBLIC ft_core PRIVATE nlohmann_json::nlohmann_json)
find_package(Threads REQUIRED)
target_link_libraries(ft_storage PUBLIC ft_core Threads::Threads PRIVATE nlohmann_json::nlohmann_json)
target_compile_definitions(ft_storage PRIVATE FT_APP_VERSION="${PROJECT_VERSION}")
target_link_libraries(ft_ui      PUBLIC ft_core ft_config ft_storage imgui)

add_subdirectory(apps/replay_cli)
add_subdirectory(apps/latency_bench)
add_subdirectory(apps/proto_common)  # portable, so it is always compiled (and warning-checked)
if(FT_BUILD_MACOS_PROTO)
  add_subdirectory(apps/macos_proto)
endif()
if(FT_BUILD_WIN_PROTO)
  add_subdirectory(apps/win_proto)
endif()

if(FT_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
