set(LIB_SLIRP_ROOT ${ANDROID_QEMU2_TOP_DIR}/../libslirp/)

set(SLIRP_VERSION_STRING "\"4.7.0\"")

set(libslirp_src
    ${LIB_SLIRP_ROOT}/src/arp_table.c
    ${LIB_SLIRP_ROOT}/src/bootp.c
    ${LIB_SLIRP_ROOT}/src/cksum.c
    ${LIB_SLIRP_ROOT}/src/dhcpv6.c
    ${LIB_SLIRP_ROOT}/src/dnssearch.c
    ${LIB_SLIRP_ROOT}/src/if.c
    ${LIB_SLIRP_ROOT}/src/ip6_icmp.c
    ${LIB_SLIRP_ROOT}/src/ip6_input.c
    ${LIB_SLIRP_ROOT}/src/ip6_output.c
    ${LIB_SLIRP_ROOT}/src/ip_icmp.c
    ${LIB_SLIRP_ROOT}/src/ip_input.c
    ${LIB_SLIRP_ROOT}/src/ip_output.c
    ${LIB_SLIRP_ROOT}/src/mbuf.c
    ${LIB_SLIRP_ROOT}/src/misc.c
    ${LIB_SLIRP_ROOT}/src/ncsi.c
    ${LIB_SLIRP_ROOT}/src/ndp_table.c
    ${LIB_SLIRP_ROOT}/src/sbuf.c
    ${LIB_SLIRP_ROOT}/src/slirp.c
    ${LIB_SLIRP_ROOT}/src/socket.c
    ${LIB_SLIRP_ROOT}/src/state.c
    ${LIB_SLIRP_ROOT}/src/stream.c
    ${LIB_SLIRP_ROOT}/src/tcp_input.c
    ${LIB_SLIRP_ROOT}/src/tcp_output.c
    ${LIB_SLIRP_ROOT}/src/tcp_subr.c
    ${LIB_SLIRP_ROOT}/src/tcp_timer.c
    ${LIB_SLIRP_ROOT}/src/tftp.c
    ${LIB_SLIRP_ROOT}/src/udp.c
    ${LIB_SLIRP_ROOT}/src/udp6.c
    ${LIB_SLIRP_ROOT}/src/util.c
    ${LIB_SLIRP_ROOT}/src/version.c
    ${LIB_SLIRP_ROOT}/src/vmstate.c)

# Generate the libslirp-version header.
configure_file(${LIB_SLIRP_ROOT}/src/libslirp-version.h.in
               ${LIB_SLIRP_ROOT}/src/libslirp-version.h)

android_add_library(
  TARGET libslirp
  LIBNAME
    libslirp
    URL
    "https://android.googlesource.com/trusty/external/qemu-libslirp/+/refs/heads/emu-master-dev"
  REPO "${LIB_SLIRP_ROOT}"
  LICENSE "BSD-3-Clause"
  SRC # cmake-format: sortable
      ${libslirp_src}
  LINUX ${LIB_SLIRP_ROOT}/src/ip_icmp_ping.c
  DARWIN ${LIB_SLIRP_ROOT}/src/ip_icmp_ping.c)

target_link_libraries(libslirp PUBLIC glib2)

android_target_link_libraries(libslirp windows PUBLIC ws2_32::ws2_32
                                                      iphlpapi::iphlpapi)

target_compile_definitions(libslirp PUBLIC -DBUILDING_LIBSLIRP)

if(WINDOWS_MSVC_X86_64)
  target_compile_definitions(
    libslirp
    PRIVATE NDEBUG=1
            _FILE_OFFSET_BITS=64
            _GNU_SOURCE=1
            _LARGEFILE_SOURCE
            _NO_EXCEPTIONS
            _USE_MATH_DEFINES
            _WIN32_WINNT=0x0601
            __STDC_FORMAT_MACROS=1
            __STDC_LIMIT_MACROS=1)
  target_compile_options(libslirp PRIVATE -Wno-format-nonliteral -Wno-unused-but-set-variable -Wno-implicit-function-declaration)
else()
  target_compile_options(
    libslirp PRIVATE -Wmissing-prototypes -Wstrict-prototypes -Wredundant-decls
                     -Wundef -Wwrite-strings)
endif()
target_include_directories(libslirp PUBLIC "${LIB_SLIRP_ROOT}/src/")
