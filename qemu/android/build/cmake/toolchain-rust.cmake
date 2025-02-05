# Copyright 2022 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE/2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# The toolchain files get processed multiple times during compile detection keep
# these files simple, and do not setup libraries or anything else. merely tags,
# compiler toolchain and flags should be set here.

# ~~~
# ! function(get_rust_version RET_VAL) : Returns the expected rust compiler version!
#
# Normally we use the rust toolchain that is provided by AOSP, however on some platforms
# (windows) we are forced to use the system provided toolchain. We want to make sure
# that we are using the same version for both.
#
# You will have to update this version if the AOSP toolchain version changes.
#
# \argn: The variable that will hold the expected rust version.
# ~~~
function(get_rust_version RET_VAL)
  file(READ "${ANDROID_QEMU2_TOP_DIR}/android/build/toolchains.json" TOOLCHAIN_JSON_STRING)
  string(JSON RUST_VERSION GET ${TOOLCHAIN_JSON_STRING} ${IDX} rust)
  set(${RET_VAL} ${RUST_VERSION} PARENT_SCOPE)
endfunction()

# ~~~
# ! function(create_windows_linker_script RET_VAL) : Returns path to a windows linker script
#
# We are using the x86_64-pc-windows-gnu target, which will invoke the linger "mingw" style
# Luckilly we can creat a wrapper that will call clang.exe with a set of additional flags
# that will make it all succeed.
#
# Note: This epxects that the CMAKE_CXX_COMPILER has already been properly configured
# and is pointing to clang
#
# ~~~
function(create_windows_linker_script)
  internal_get_env_cache(WINDOWS_LINK_SCRIPT)
  internal_get_env_cache(RUST_LINK_PATH)
  if("${WINDOWS_LINK_SCRIPT}" STREQUAL "")

    get_clang_version(CLANG_VER)
    get_filename_component(
      CLANG_DIR
      "${ANDROID_QEMU2_TOP_DIR}/../../prebuilts/clang/host/windows-x86/${CLANG_VER}"
      ABSOLUTE)
    get_filename_component(
      CLANG_LINUX_DIR
      "${ANDROID_QEMU2_TOP_DIR}/../../prebuilts/clang/host/linux-x86/${CLANG_VER}"
      ABSOLUTE)
    get_filename_component(
      MINGW_DIR
      "${ANDROID_QEMU2_TOP_DIR}/../../prebuilts/gcc/linux-x86/host/x86_64-w64-mingw32-4.8"
      ABSOLUTE)

    set(TOOLCHAIN "${PROJECT_BINARY_DIR}/toolchain")
    file(MAKE_DIRECTORY ${TOOLCHAIN})
    set(TOOLCHAIN "${PROJECT_BINARY_DIR}/toolchain")
    file(MAKE_DIRECTORY ${TOOLCHAIN})
    file(MAKE_DIRECTORY ${TOOLCHAIN}/lib)

    # Construct the -L parameters for clang.exe using the LIB environment
    # variable This should be available since we should be in an msvc shell.
    string(REPLACE ";" "\" -L\"" LIB_PARAMS "$ENV{LIB}")
    set(LIB_PARAMS "\"-L${LIB_PARAMS}\"")

    file(
      WRITE ${TOOLCHAIN}/ld-emu.cmd
      "${CLANG_DIR}/bin/clang.exe ^
      --target=x86_64-pc-windows-gnu ^
      --sysroot=${MINGW_DIR}/x86_64-w64-mingw32 ^
      -B${MINGW_DIR}/lib/gcc/x86_64-w64-mingw32/4.8.3 ^
      -L${MINGW_DIR}/lib/gcc/x86_64-w64-mingw32/4.8.3 ^
      -L${MINGW_DIR}/x86_64-w64-mingw32/lib64 ^
      -L${CLANG_DIR}/lib -Wl,-mllvm,--relocation-model=pic ^
      -fuse-ld=lld ^
      -L${LIB_PARAMS} ^
      %*")
    execute_process(
      COMMAND
        python
        "${ANDROID_QEMU2_TOP_DIR}/android/build/python/aemu/util/mingw_to_msvc_lib.py"
        ${MINGW_DIR}/lib/gcc/x86_64-w64-mingw32/4.8.3/libgcc_eh.a
        ${TOOLCHAIN}/lib/gcc_eh.lib)
    execute_process(
      COMMAND
        python
        "${ANDROID_QEMU2_TOP_DIR}/android/build/python/aemu/util/mingw_to_msvc_lib.py"
        ${MINGW_DIR}/x86_64-w64-mingw32/lib64/libpthread.a
        ${TOOLCHAIN}/lib/pthread.lib)

    string(REPLACE "/" "\\" RUST_LINK_PATH
                   "${PROJECT_BINARY_DIR}/toolchain/lib")


    internal_set_env_cache(WINDOWS_LINK_SCRIPT "${TOOLCHAIN}/ld-emu.cmd")
    internal_set_env_cache(RUST_LINK_PATH "${RUST_LINK_PATH}")
  endif()
  set(WINDOWS_LINK_SCRIPT "${WINDOWS_LINK_SCRIPT}" PARENT_SCOPE)
  set(RUST_LINK_PATH "${RUST_LINK_PATH}" PARENT_SCOPE)
  message(STATUS "Created ${WINDOWS_LINK_SCRIPT}, with path ${RUST_LINK_PATH} for linking")
endfunction()
# ~~~
# ! enable_vendorized_crates : writes a cargo crate config in the ${Rust_ROOT}!
#
# This writes a config.toml that informs rust to use vendorized crates from the
# first parameter. The config.toml will be written to the ${Rust_ROOT}
# directory.
#
# This function will set the Rust_CARGO_HOME variable and CARGO_HOME environment
# variable that is used by corrosion to configure cargo.
#
# \argn: Directory where the vendored crates can be found
# ~~~
function(enable_vendorized_crates VENDOR_CRATES)
  set(Rust_ROOT "${CMAKE_BINARY_DIR}/rust")
  set(CARGO_HOME "${Rust_ROOT}/.cargo")
  set(CARGO_CONFIG "${CARGO_HOME}/config.toml")
  message(
    STATUS
      "Writing ${CARGO_CONFIG}, cargo should now use vendor crates from ${VENDOR_CRATES}"
  )
  configure_file("${ANDROID_QEMU2_TOP_DIR}/android/build/cmake/config.toml.in"
                 ${CARGO_CONFIG})
  set(Rust_CARGO_HOME ${CARGO_HOME} PARENT_SCOPE)
  set(ENV{CARGO_HOME} ${CARGO_HOME})
endfunction()

# ~~~
# ! configure_rust : Configures the rust compiler that is found in the given
# directory!
#
# This sets a series of variables, (Rust_COMPILER, Rust_Cargo)
# that are used to compile rust libraries, and will write the
# cargo configuration if we are using vendorized crates
#
# :RUST_COMPILER_ROOT: The directory where the rustc compiler can be found.
# :VENDOR_CRATESs:     The optional directory where we can find the
#                      vendorized crates
# ~~~
function(configure_rust)
  set(options)
  set(oneValueArgs COMPILER_ROOT VENDOR_CRATES)
  set(multiValueArg)
  cmake_parse_arguments(cfg "${options}" "${oneValueArgs}" "${multiValueArgs}"
                        ${ARGN})

  # This function can get called multiple times during toolchain configuration,
  # to make sure we only get invoked one time we cache this result.
  internal_get_env_cache(Rust_CARGO)
  if(Rust_CARGO)
    # We have already been configured.
    return()
  endif()

  if(EXISTS "${cfg_COMPILER_ROOT}")
    set(Rust_COMPILER ${cfg_COMPILER_ROOT}/rustc PARENT_SCOPE)
    set(Rust_CARGO ${cfg_COMPILER_ROOT}/cargo PARENT_SCOPE)
    internal_set_env_cache(Rust_CARGO "${RUST_COMPILER_ROOT}/cargo")
  else()
    set(Rust_CARGO "NOT_FOUND" PARENT_SCOPE)
    set(Rust_COMPILER "NOT_FOUND" PARENT_SCOPE)
    internal_set_env_cache(Rust_CARGO "${RUST_COMPILER_ROOT}/cargo")
    message(
      WARNING
        "\n\n=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=\n"
        "No rust toolchain available!"
        "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=\n\n"
    )
  endif()

  if(cfg_VENDOR_CRATES)
    enable_vendorized_crates(cfg_VENDOR_CRATES)
  endif()
endfunction()

# ~~~
# ! use_system_rust_toolchain : Unsets the rust variables, so they will be derived!
#
# This will force corrosion to infer the variables, must be executed before
# including corrosion.
# ~~~
function(use_system_rust_toolchain)
  if(CORROSION_CMAKE_INCLUDED)
    message(FATAL_ERROR "This should be called before including corrosion.")
  endif()
  unset(Rust_COMPILER PARENT_SCOPE)
  unset(Rust_CARGO PARENT_SCOPE)
endfunction(use_system_rust_toolchain)

# ~~~
# ! ensure_rust_version_is_compliant : Checks the configured rust toolchain matches the expected version!
#
# This makes sure the Rust_VERSION variable contains the expected value.
# ~~~
function(ensure_rust_version_is_compliant)
  get_rust_version(EXPECTED_VERSION)
  if(NOT Rust_VERSION)
    message(
      FATAL_ERROR
        "Rust compiler toolchain not initialized, did you not run add_subdirectory(${ANDROID_QEMU2_TOP_DIR}/android/build/cmake/corrosion)"
    )
  endif()
  if(NOT Rust_VERSION VERSION_EQUAL EXPECTED_VERSION)
    message(
      FATAL_ERROR
        "\n\n=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=\n"
        "Expected version rust compiler to provide: ${EXPECTED_VERSION}, not ${Rust_VERSION}, \n"
        "please update ${Rust_COMPILER} to the expected version: ${EXPECTED_VERSION} \n"
        "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=\n\n"
    )
  endif()
endfunction()
