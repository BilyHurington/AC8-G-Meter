include(FetchContent)
set(FETCHCONTENT_QUIET ON)

# Dear ImGui (no CMake upstream; we define the target ourselves).
FetchContent_Declare(imgui_src
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG v1.92.9
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(imgui_src)
add_library(imgui STATIC
  ${imgui_src_SOURCE_DIR}/imgui.cpp
  ${imgui_src_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_src_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_src_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_src_SOURCE_DIR}/imgui_demo.cpp)
target_include_directories(imgui SYSTEM PUBLIC ${imgui_src_SOURCE_DIR} ${imgui_src_SOURCE_DIR}/backends)
target_compile_definitions(imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
set(FT_IMGUI_DIR ${imgui_src_SOURCE_DIR} CACHE INTERNAL "")
if(WIN32)
  # Win32 + Direct3D 12 backends: the Windows prototype and the in-game overlay (AC8 is DX12).
  add_library(imgui_win32_dx12 STATIC
    ${imgui_src_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_src_SOURCE_DIR}/backends/imgui_impl_dx12.cpp)
  target_link_libraries(imgui_win32_dx12 PUBLIC imgui d3d12 dxgi d3dcompiler)
  target_compile_definitions(imgui_win32_dx12 PRIVATE UNICODE _UNICODE)
endif()

FetchContent_Declare(nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz)
FetchContent_MakeAvailable(nlohmann_json)

if(FT_BUILD_TESTS)
  FetchContent_Declare(doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG v2.5.2
    GIT_SHALLOW TRUE)
  set(DOCTEST_NO_INSTALL ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(doctest)
endif()
