enable_language(OBJCXX)
find_package(SDL3 CONFIG REQUIRED)

add_executable(ac8_gmeter_proto main.mm
  ${FT_IMGUI_DIR}/backends/imgui_impl_sdl3.cpp
  ${FT_IMGUI_DIR}/backends/imgui_impl_metal.mm)
target_link_libraries(ac8_gmeter_proto PRIVATE ft_proto_common SDL3::SDL3
  "-framework Metal" "-framework QuartzCore" "-framework Cocoa" "-framework ImageIO" "-framework CoreGraphics")
# Third-party backends: ARC for the Metal one, and no first-party warning flags on either.
set_source_files_properties(${FT_IMGUI_DIR}/backends/imgui_impl_metal.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc;-w")
set_source_files_properties(${FT_IMGUI_DIR}/backends/imgui_impl_sdl3.cpp PROPERTIES COMPILE_OPTIONS "-w")
target_compile_options(ac8_gmeter_proto PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
target_compile_definitions(ac8_gmeter_proto PRIVATE FT_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
ft_warnings(ac8_gmeter_proto)
ft_copy_proto_assets(ac8_gmeter_proto)
