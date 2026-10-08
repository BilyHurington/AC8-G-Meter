enable_language(OBJCXX)
find_package(SDL3 CONFIG REQUIRED)

file(GLOB _proto_srcs CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.mm ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/*.h)
add_executable(ft_macos_proto ${_proto_srcs}
  ${FT_IMGUI_DIR}/backends/imgui_impl_sdl3.cpp
  ${FT_IMGUI_DIR}/backends/imgui_impl_metal.mm)
target_link_libraries(ft_macos_proto PRIVATE ft_ui ft_sources ft_config ft_core imgui SDL3::SDL3
  "-framework Metal" "-framework QuartzCore" "-framework Cocoa")
set_source_files_properties(${FT_IMGUI_DIR}/backends/imgui_impl_metal.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
target_compile_options(ft_macos_proto PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
target_compile_definitions(ft_macos_proto PRIVATE FT_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
ft_warnings(ft_macos_proto)

# Fonts and sample replays next to the binary so the prototype runs from the build dir.
add_custom_command(TARGET ft_macos_proto POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/assets $<TARGET_FILE_DIR:ft_macos_proto>/assets
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/data/replays $<TARGET_FILE_DIR:ft_macos_proto>/replays)
