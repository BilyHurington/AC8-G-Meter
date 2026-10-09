# AC8GMeter-Installer.exe: a single file that installs the mod (and the bundled UE4SS when the
# game has none) into ACE COMBAT 8, and uninstalls it. The files are embedded as a resource
# packed by ft_pack_payload at build time.
include(${CMAKE_SOURCE_DIR}/cmake/Ue4ssPayload.cmake)

add_library(ft_installer_core STATIC payload.cpp payload.h install.cpp install.h)
target_include_directories(ft_installer_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ft_installer_core PUBLIC ft_core PRIVATE nlohmann_json::nlohmann_json cabinet bcrypt version)
target_compile_definitions(ft_installer_core PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
ft_warnings(ft_installer_core)

add_executable(ft_pack_payload pack_main.cpp)
target_link_libraries(ft_pack_payload PRIVATE ft_installer_core)
ft_warnings(ft_pack_payload)

# Payload: the vendored UE4SS ("ue4ss/" -> Game\Binaries\Win64, with our list of its third-party
# components next to UE4SS.dll, so it stays when only the mod is uninstalled) and the mod folder
# assembled by ac8gmeter_package ("mod/" -> ...\Mods\AC8GMeter).
set(FT_PAYLOAD_FILE ${CMAKE_CURRENT_BINARY_DIR}/payload.bin)
file(GLOB _mod_sources CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/mod/lua/Scripts/*.lua ${CMAKE_SOURCE_DIR}/mod/package/*)
add_custom_command(OUTPUT ${FT_PAYLOAD_FILE}
  COMMAND ft_pack_payload ${FT_PAYLOAD_FILE} ue4ss=${FT_UE4SS_PAYLOAD_DIR} ue4ss/UE4SS=${FT_UE4SS_NOTICES_DIR}
          mod=${FT_MOD_PACKAGE_DIR}
  DEPENDS ft_pack_payload ac8gmeter $<TARGET_FILE:ac8gmeter> ${FT_UE4SS_PAYLOAD_FILES}
          ${FT_UE4SS_NOTICES_DIR}/THIRD_PARTY_LICENSES.txt ${_mod_sources}
  COMMENT "Packing the installer payload")
add_custom_target(ac8gmeter_payload DEPENDS ${FT_PAYLOAD_FILE})
add_dependencies(ac8gmeter_payload ac8gmeter_package)  # the mod folder is assembled first

configure_file(installer.rc.in ${CMAKE_CURRENT_BINARY_DIR}/installer.rc @ONLY)
add_executable(ac8gmeter_installer WIN32 main.cpp installer_resources.h installer.manifest
               ${CMAKE_CURRENT_BINARY_DIR}/installer.rc)
set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/installer.rc PROPERTIES OBJECT_DEPENDS ${FT_PAYLOAD_FILE})
target_include_directories(ac8gmeter_installer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ac8gmeter_installer PRIVATE ft_installer_core comctl32 ole32 shell32)
target_compile_definitions(ac8gmeter_installer PRIVATE FT_APP_VERSION="${PROJECT_VERSION}")
# The file players download, at the top of the build directory.
set_target_properties(ac8gmeter_installer PROPERTIES OUTPUT_NAME AC8GMeter-Installer
                      RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
add_dependencies(ac8gmeter_installer ac8gmeter_payload)
ft_warnings(ac8gmeter_installer)
