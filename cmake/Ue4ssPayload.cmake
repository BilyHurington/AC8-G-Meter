# The vendored UE4SS runtime (third_party/ue4ss/payload) that the installer ships. Every file
# is checked against the SHA-256 recorded in third_party/ue4ss/README.md, so an accidental
# change (e.g. line-ending conversion by git) fails the configure step.
set(FT_UE4SS_PAYLOAD_DIR ${CMAKE_SOURCE_DIR}/third_party/ue4ss/payload)
# Our list of the components built into UE4SS.dll (with license texts), installed next to it.
set(FT_UE4SS_NOTICES_DIR ${CMAKE_SOURCE_DIR}/third_party/ue4ss/notices)
set(_ft_ue4ss_hashes
  "dwmapi.dll=cf440b9eb8643bb7c434acfda696aee57fd981d185dca5e57fb8dbb18f8fc1cd"
  "override.txt=3f49fec48a6980e3df3dbf85dda29577dfb65a48af76755c11922e9f3d276a94"
  "UE4SS/UE4SS.dll=680a026890abb4d0df2211251f8defc1681a584275f1521dcc0fe30af480006f"
  "UE4SS/UE4SS-settings.ini=88962063029d9f1b67b1eebb903b0c8509a7864e81c70ac03e13d99d07542539"
  "UE4SS/UE4SS_SDK_Backends/UE4SS.json=2c309da34012ed0cc0d3a4b6a34c8287ec6b0011b1f864fd3e8424edff4fb154"
  "UE4SS/LICENSE=ddc030e25d0ea87aca4ae84c0ed3f868d69273c00c0c12ea1e26f1c6130f5d2e")
set(FT_UE4SS_PAYLOAD_FILES "")
foreach(_entry ${_ft_ue4ss_hashes})
  string(REPLACE "=" ";" _parts "${_entry}")
  list(GET _parts 0 _file)
  list(GET _parts 1 _expected)
  set(_path ${FT_UE4SS_PAYLOAD_DIR}/${_file})
  if(NOT EXISTS ${_path})
    message(FATAL_ERROR "UE4SS payload file missing: ${_path}")
  endif()
  file(SHA256 ${_path} _actual)
  if(NOT _actual STREQUAL _expected)
    message(FATAL_ERROR "UE4SS payload file changed: ${_file}\n  expected ${_expected}\n  actual   ${_actual}")
  endif()
  list(APPEND FT_UE4SS_PAYLOAD_FILES ${_path})
endforeach()
