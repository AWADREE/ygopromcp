project "YGOProUpdateLauncher"
    kind "WindowedApp"
    language "C++"
    rtti "Off"
    files { "UpdateLauncher.cpp", "FirstRunSetup.cpp", "FirstRunSetup.h",
        "NativeUpdater.cpp", "NativeUpdater.h", "UpdateLauncher.rc" }
    includedirs { "../../vendor", SQLITE_INCLUDE_DIR, ZLIB_INCLUDE_DIR }
    links { "bcrypt", "user32", "shell32", "comctl32", "winhttp" }
    if BUILD_SQLITE then
        links { "sqlite" }
    else
        links { SQLITE_LIB_NAME }
        libdirs { SQLITE_LIB_DIR }
    end
    if BUILD_ZLIB then
        links { "zlib" }
    else
        links { ZLIB_LIB_NAME }
        libdirs { ZLIB_LIB_DIR }
    end
    filter "not action:vs*"
        cppdialect "C++14"
    filter { "system:windows", "action:gmake" }
        linkoptions { "-municode" }
    filter "system:windows"
        defines { "NOMINMAX=1", "WIN32_LEAN_AND_MEAN" }
    filter {}
