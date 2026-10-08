# One doctest executable per layer so each layer's tests build independently.
function(ft_test_suite name)
  file(GLOB _srcs CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${name}/*.cpp)
  add_executable(ft_${name}_tests ${_srcs})
  target_link_libraries(ft_${name}_tests PRIVATE ${ARGN} doctest::doctest)
  target_compile_definitions(ft_${name}_tests PRIVATE
    FT_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    FT_TEST_TMP_DIR="${CMAKE_BINARY_DIR}/test_tmp")
  ft_warnings(ft_${name}_tests)
  add_test(NAME ft_${name}_tests COMMAND ft_${name}_tests)
endfunction()

file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/test_tmp)
ft_test_suite(core    ft_core)
ft_test_suite(sources ft_sources)
ft_test_suite(storage ft_storage)
ft_test_suite(ui      ft_ui ft_config)

# End-to-end regression: replay recorded flights and check estimates against ground truth.
file(GLOB _replays CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/data/replays/*.csv)
foreach(_csv ${_replays})
  get_filename_component(_n ${_csv} NAME_WE)
  add_test(NAME replay_${_n} COMMAND replay_cli ${_csv} --check)
endforeach()
# --save-report writes a loadable mission archive pair (checked by its exit status).
add_test(NAME replay_save_report COMMAND replay_cli ${CMAKE_SOURCE_DIR}/data/replays/dogfight_mix.csv
         --save-report ${CMAKE_BINARY_DIR}/test_tmp/replay_archive)
