cc_library(
    name = "compat-hdrs",
    hdrs = glob(["includes/**/*.h"]),
    defines = ["AEMU_WIN_COMPAT"],
    includes = [
        "includes",
        "includes/dirent",
    ],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "compat",
    srcs =
        glob([
            "src/dirent/*.c",
            "src/*.c",
            "src/*.h",
            "src/*.cpp",
        ]),
    defines = [
        "WIN32_LEAN_AND_MEAN",
    ],
    includes = [
        "src",
    ],
    linkopts = [
        "-DEFAULTLIB:ws2_32.lib",
        "-DEFAULTLIB:Pathcch.lib",
        "-DEFAULTLIB:ole32.lib",
        "-DEFAULTLIB:dxguid.lib",
        "-DEFAULTLIB:Winmm.lib",
    ],
    linkstatic = True,
    visibility = ["//visibility:public"],
    deps = [":compat-hdrs"],
)
