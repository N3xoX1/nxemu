# Older CMake can evaluate the Android NDK version too early and inject gold
# into IPO links, even with an NDK whose actual linker is LLD. Retain that
# toolchain's selected linker instead of requesting a removed gold executable.
if(ANDROID AND CMAKE_LINKER MATCHES "[/\\\\](ld\\.)?lld(\\.exe)?$")
    foreach(_lang C CXX)
        list(REMOVE_ITEM CMAKE_${_lang}_LINK_OPTIONS_IPO "-fuse-ld=gold")
    endforeach()
endif()
