# Offline test of the in-game overlay (mod/overlay): a Win32 + Direct3D 12 "game" that clears
# and presents its own swap chain while ft_mod_overlay hooks Present and draws into it.
add_executable(overlay_harness main.cpp)
target_link_libraries(overlay_harness PRIVATE ft_mod_overlay d3d11)  # d3d11: the --decoys window
ft_warnings(overlay_harness)

if(FT_BUILD_TESTS)
  # This directory is added before the top level calls enable_testing(); without enabling it
  # here too its tests would not be written to CTestTestfile.cmake.
  enable_testing()
  # Each run opens a window for about a second and checks the presented pixels and the input
  # routing (--check). See main.cpp for what each option exercises.
  set(_cases
    "bgra8|--format bgra8"
    "rgba8|--format rgba8"
    "rgb10a2|--format rgb10a2"
    "rgb10a2_hdr10|--format rgb10a2 --hdr10"
    # The colour space set before the overlay starts, as UE does: found by memory discovery.
    "rgb10a2_hdr10_early|--format rgb10a2 --hdr10-early"
    "fp16|--format fp16"
    "resize|--format bgra8 --resize-at 45 --frames 90"
    "present1|--format bgra8 --present1"
    # Window thread != present thread, as in the game.
    "render_thread|--format bgra8 --render-thread --resize-at 45 --frames 90"
    # Client area smaller than the back buffer: mouse positions are scaled.
    "scaled|--format bgra8 --size 1280x720 --window 640x360"
    # A D3D11 window, a hidden D3D12 swap chain and DXGI_PRESENT_TEST must be ignored.
    "decoys|--format bgra8 --decoys"
    # Another mod restores the window procedure we replaced.
    "resubclass|--format bgra8 --unsubclass-at 5"
    # A structured exception in the client, and disableOverlay(): the overlay must stop and
    # the "game" keep running with all its input.
    "fault|--format bgra8 --fault-at 3"
    "disable|--format bgra8 --disable-at 5")
  foreach(_case ${_cases})
    string(REPLACE "|" ";" _parts "${_case}")
    list(GET _parts 0 _name)
    list(GET _parts 1 _args)
    separate_arguments(_args)
    add_test(NAME overlay_harness_${_name} COMMAND overlay_harness ${_args} --check)
    set_tests_properties(overlay_harness_${_name} PROPERTIES TIMEOUT 60)
  endforeach()
endif()
