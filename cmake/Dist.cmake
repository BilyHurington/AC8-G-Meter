# Release files (target ac8gmeter_dist), in build/dist:
#   AC8GMeter-<version>-Full.zip       UE4SS + the mod, laid out from the game folder (Game\...)
#   AC8GMeter-<version>-ModOnly.zip    the mod folder only, for an existing UE4SS (...\Mods\)
#   AC8GMeter-<version>-Installer.exe  AC8GMeter-Installer.exe (installs / updates / uninstalls)
# <version>: FT_DIST_VERSION (CI passes the tag), else the project version.
include(${CMAKE_CURRENT_LIST_DIR}/Ue4ssPayload.cmake)  # FT_UE4SS_* (hash-checked)
set(FT_DIST_VERSION "${PROJECT_VERSION}" CACHE STRING "Version in the names of the release files")
set(_dist ${CMAKE_BINARY_DIR}/dist)
set(_full ${_dist}/stage/full)
set(_modonly ${_dist}/stage/modonly)
set(_win64 ${_full}/Game/Binaries/Win64)
set(_name AC8GMeter-${FT_DIST_VERSION})
set(_readme ${CMAKE_SOURCE_DIR}/mod/package/README.txt)

add_custom_target(ac8gmeter_dist
  COMMAND ${CMAKE_COMMAND} -E remove_directory ${_dist}
  # Full: the vendored UE4SS (with our list of its components) and the mod inside it.
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${FT_UE4SS_PAYLOAD_DIR} ${_win64}
  COMMAND ${CMAKE_COMMAND} -E copy ${FT_UE4SS_NOTICES_DIR}/THIRD_PARTY_LICENSES.txt ${_win64}/UE4SS/
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${FT_MOD_PACKAGE_DIR} ${_win64}/UE4SS/Mods/AC8GMeter
  COMMAND ${CMAKE_COMMAND} -E copy ${_readme} ${_full}/AC8GMeter-README.txt
  COMMAND ${CMAKE_COMMAND} -E chdir ${_full}
          ${CMAKE_COMMAND} -E tar cf ${_dist}/${_name}-Full.zip --format=zip Game AC8GMeter-README.txt
  # ModOnly: the mod folder for ...\<UE4SS folder>\Mods\.
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${FT_MOD_PACKAGE_DIR} ${_modonly}/AC8GMeter
  COMMAND ${CMAKE_COMMAND} -E copy ${_readme} ${_modonly}/AC8GMeter-README.txt
  COMMAND ${CMAKE_COMMAND} -E chdir ${_modonly}
          ${CMAKE_COMMAND} -E tar cf ${_dist}/${_name}-ModOnly.zip --format=zip AC8GMeter AC8GMeter-README.txt
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:ac8gmeter_installer> ${_dist}/${_name}-Installer.exe
  COMMAND ${CMAKE_COMMAND} -E remove_directory ${_dist}/stage
  DEPENDS ac8gmeter_installer ac8gmeter_package
  COMMENT "Release files in ${_dist}")
