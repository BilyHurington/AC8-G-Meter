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
if(FT_BUILD_MOD)
  ft_test_suite(mod   ft_mod_flight)
endif()

# End-to-end regression: replay recorded flights and check estimates against ground truth.
file(GLOB _replays CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/data/replays/*.csv)
foreach(_csv ${_replays})
  get_filename_component(_n ${_csv} NAME_WE)
  add_test(NAME replay_${_n} COMMAND replay_cli ${_csv} --check)
endforeach()
# --save-report writes a loadable mission archive pair (checked by its exit status).
add_test(NAME replay_save_report COMMAND replay_cli ${CMAKE_SOURCE_DIR}/data/replays/dogfight_mix.csv
         --save-report ${CMAKE_BINARY_DIR}/test_tmp/replay_archive)

# End-to-end test of the mod DLL with a fake UE4SS.dll (Lua wrapper exports on a fake stack).
if(FT_BUILD_MOD)
  add_library(fake_ue4ss SHARED mod_e2e/fake_ue4ss.cpp mod_e2e/fake_lua.h)
  set_target_properties(fake_ue4ss PROPERTIES OUTPUT_NAME UE4SS RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/fake_ue4ss)
  ft_warnings(fake_ue4ss)
  add_executable(ft_mod_e2e_tests mod_e2e/mod_e2e_tests.cpp)
  target_link_libraries(ft_mod_e2e_tests PRIVATE ft_mod_flight ft_sources doctest::doctest)
  target_compile_definitions(ft_mod_e2e_tests PRIVATE
    FT_TEST_TMP_DIR="${CMAKE_BINARY_DIR}/test_tmp"
    FT_MOD_DLL="$<TARGET_FILE:ac8gmeter>"
    FT_FAKE_UE4SS="$<TARGET_FILE:fake_ue4ss>")
  add_dependencies(ft_mod_e2e_tests ac8gmeter fake_ue4ss)
  ft_warnings(ft_mod_e2e_tests)
  add_test(NAME ft_mod_e2e_tests COMMAND ft_mod_e2e_tests)
endif()
