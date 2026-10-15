add_library(main SHARED
  AchievementAdapter.cpp
  ActivityTracker.cpp
  Cheats/ARCheat.cpp
  Cheats/Cheats.h
  Cheats/GeckoCheat.cpp
  Cheats/GraphicsMod.cpp
  Cheats/GraphicsModGroup.cpp
  Cheats/PatchCheat.cpp
  Config/ConfigChangedCallback.cpp
  Config/NativeConfig.cpp
  Config/PostProcessing.cpp
  GameList/GameFile.cpp
  GameList/GameFile.h
  GameList/GameFileCache.cpp
  GpuDriver.cpp
  Host.cpp
  Host.h
  InfinityConfig.cpp
  Input/Control.cpp
  Input/Control.h
  Input/ControlGroup.cpp
  Input/ControlGroup.h
  Input/ControlReference.cpp
  Input/ControlReference.h
  Input/CoreDevice.cpp
  Input/CoreDevice.h
  Input/EmulatedController.cpp
  Input/EmulatedController.h
  Input/HotkeyDispatcher.cpp
  Input/HotkeyDispatcher.h
  Input/InputDetector.cpp
  Input/InputOverrider.cpp
  Input/MappingCommon.cpp
  Input/NumericSetting.cpp
  Input/NumericSetting.h
  MainAndroid.cpp
  RiivolutionPatches.cpp
  SkylanderConfig.cpp
  StreetOnline.cpp
  StreetOnline.h
  StreetVoice.cpp
  StreetVoice.h
  WiiUtils.cpp
)

# StreetMR r35: Opus (BSD-3-Clause, Externals/opus = libopus 1.5.2) for online voice chat.
set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
# Dolphin turns CTest's BUILD_TESTING on; Opus would then add its test programs (not shipped here).
set(STREET_SAVED_BUILD_TESTING ${BUILD_TESTING})
set(BUILD_TESTING OFF)
add_subdirectory(${CMAKE_SOURCE_DIR}/Externals/opus ${CMAKE_BINARY_DIR}/Externals/opus
                 EXCLUDE_FROM_ALL)
set(BUILD_TESTING ${STREET_SAVED_BUILD_TESTING})
set_target_properties(opus PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(main PRIVATE opus)

target_link_libraries(main
PRIVATE
  androidcommon
  common
  core
  inputcommon
  uicommon
)

target_link_libraries(main
PRIVATE
  android
  log
  "-Wl,--no-warn-mismatch"
  "-Wl,--whole-archive"
  "-Wl,--no-whole-archive"
)

target_include_directories(main
PRIVATE
    ${CMAKE_SOURCE_DIR}/Externals/libadrenotools/include
    ${CMAKE_SOURCE_DIR}/Externals/VulkanMemoryAllocator/include
)

file(GLOB_RECURSE android_sys_assets CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/Data/Sys/*")
file(MAKE_DIRECTORY ${CMAKE_SOURCE_DIR}/Source/Android/app/src/main/assets/)
file(REMOVE_RECURSE ${CMAKE_SOURCE_DIR}/Source/Android/app/src/main/assets/Sys/)
file(COPY ${CMAKE_SOURCE_DIR}/Data/Sys DESTINATION ${CMAKE_SOURCE_DIR}/Source/Android/app/src/main/assets/)

# Delete folders that aren't used by the Android version of Dolphin
file(REMOVE_RECURSE ${CMAKE_SOURCE_DIR}/Source/Android/app/src/main/assets/Sys/Themes/)

set(CPACK_PACKAGE_EXECUTABLES ${CPACK_PACKAGE_EXECUTABLES} main)

add_subdirectory(AndroidCommon)
