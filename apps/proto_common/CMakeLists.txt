# Prototype logic shared by the platform hosts (macos_proto, win_proto): simulator / replay
# source, pipeline, fake game scene, debug panel, command line. ImGui and project libraries
# only: no window system or graphics API.
file(GLOB _common_srcs CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/*.h)
add_library(ft_proto_common STATIC ${_common_srcs})
target_include_directories(ft_proto_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ft_proto_common PUBLIC ft_ui ft_sources ft_config ft_storage ft_core imgui)
ft_warnings(ft_proto_common)

# Copies fonts and sample replays next to a host binary so it runs from the build dir.
function(ft_copy_proto_assets target)
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/assets $<TARGET_FILE_DIR:${target}>/assets
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/data/replays $<TARGET_FILE_DIR:${target}>/replays)
endfunction()
