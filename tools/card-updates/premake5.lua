project "YGOProUpdateLauncher"
    kind "WindowedApp"
    language "C++"
    rtti "Off"
    files { "UpdateLauncher.cpp", "FirstRunSetup.cpp", "FirstRunSetup.h",
        "NativeUpdater.cpp", "NativeUpdater.h", "UpdateLauncher.rc",
        "UpdateLauncher.manifest" }
    includedirs { "../../vendor", SQLITE_INCLUDE_DIR, ZLIB_INCLUDE_DIR }
    links { "bcrypt", "user32", "shell32", "comctl32", "winhttp", SQLITE_LIB_NAME, ZLIB_LIB_NAME }
    libdirs { SQLITE_LIB_DIR, ZLIB_LIB_DIR }
    filter "not action:vs*"
        cppdialect "C++14"
    filter { "system:windows", "action:gmake" }
        linkoptions { "-municode" }
    filter "system:windows"
        defines { "NOMINMAX=1", "WIN32_LEAN_AND_MEAN" }
    filter {}
