cmake_minimum_required(VERSION 3.5)

# Generate the VirtioGpuTimelines flatbuffers header
add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/VirtioGpuTimelines_generated.h
  COMMAND flatc ARGS --cpp --gen-object-api --scoped-enums -o
          ${CMAKE_CURRENT_BINARY_DIR} emulation/VirtioGpuTimelines.fbs
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  DEPENDS flatc emulation/VirtioGpuTimelines.fbs)
add_custom_target(
  virtio_gpu_timelines_fbs_header ALL
  DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/VirtioGpuTimelines_generated.h)

add_subdirectory(netsim)

set(android-qemu2-glue_src
    android_qemud.cpp
    audio-capturer.cpp
    audio-output.cpp
    base/async/CpuLooper.cpp
    base/async/Looper.cpp
    base/files/QemuFileStream.cpp
    base/files/KernelCmdLineLoader.cpp
    display.cpp
    drive-share.cpp
    dtb.cpp
    emulation/CharSerialLine.cpp
    emulation/DmaMap.cpp
    emulation/VmLock.cpp
    emulation/android_address_space_device.cpp
    emulation/android_pipe_device.cpp
    emulation/charpipe.c
    emulation/goldfish_sync.cpp
    emulation/serial_line.cpp
    emulation/virtio-input-multi-touch.c
    emulation/virtio-input-rotary.c
    emulation/virtio_vsock_device.cpp
    emulation/virtio_vsock_transport.cpp
    emulation/virtio-wifi.cpp
    netsim/NetsimWifiForwarder.cpp
    emulation/VirtioGpuTimelines.cpp
    emulation/VirtioWifiForwarder.cpp
    emulation/WifiService.cpp
    looper-qemu.cpp
    net-android.cpp
    proxy/slirp_proxy.cpp
    qemu-automation-agent-impl.cpp
    qemu-battery-agent-impl.cpp
    qemu-car-data-agent-impl.cpp
    qemu-cellular-agent-impl.c
    qemu-console-factory.cpp
    qemu-clipboard-agent-impl.cpp
    qemu-display-agent-impl.cpp
    qemu-finger-agent-impl.c
    qemu-globals-agent-impl.cpp
    qemu-grpc-agent-impl.cpp
    qemu-http-proxy-agent-impl.c
    qemu-hw-control-agent-impl.cpp
    qemu-location-agent-impl.c
    qemu-multi-display-agent-impl.cpp
    qemu-net-agent-impl.c
    qemu-record-screen-agent-impl.c
    qemu-sensors-agent-impl.cpp
    qemu-setup-dns-servers.cpp
    qemu-setup.cpp
    netsim/h4_parser.cc
    netsim/BluetoothPacketProtocol.cpp
    netsim/UwbPacketProtocol.cpp
    netsim/PacketStreamTransport.cpp
    netsim/qemu-packet-stream-agent-impl.cpp
    qemu-telephony-agent-impl.c
    qemu-user-event-agent-impl.c
    qemu-virtual-scene-agent-impl.cpp
    snapshot_compression.cpp
    telephony/modem_init.c
    utils/register_crash_info.cpp
    utils/stream.cpp)

if(OPTION_GFXSTREAM_BACKEND)
  set(android-qemu2-virtio_src emulation/virtio-gpu-gfxstream-renderer.cpp)
  set(android-qemu2-virtio_hdr gfxstream_backend_headers)
else()
  set(android-qemu2-virtio_src emulation/virtio-goldfish-pipe.cpp)
endif()

android_add_library(TARGET libqemu2-glue LICENSE Apache-2.0
                    SRC ${android-qemu2-glue_src} ${android-qemu2-virtio_src})

target_include_directories(
  libqemu2-glue
  PRIVATE ..
          ../include
          ${ANDROID_AUTOGEN}
          ../slirp
          ${VIRGLRENDERER_INCLUDE_DIR}
          ${ANDROID_QEMU2_TOP_DIR}/android/third_party/libdtb/include/
          ${CMAKE_CURRENT_BINARY_DIR})

target_link_libraries(
  libqemu2-glue PRIVATE android-grpc-services-stack ui::gl-bridge
                        android-grpc-interceptors ${android-qemu2-virtio_hdr})

if(WEBRTC)
  target_link_libraries(libqemu2-glue PRIVATE android-webrtc)
endif()

target_link_libraries(
  libqemu2-glue
  PRIVATE emulated-bluetooth-service-server-impl grpc++
          packet-streamer-client-lib packet-streamer-proto-lib
          protobuf::libprotobuf)

target_link_libraries(
  libqemu2-glue
  PRIVATE android-emu
          hostapd
          glib2
          PIXMAN::PIXMAN
          emulator-libdtb
          android-emu-gps
          ssl
          flatbuffers
          qemu-host-common-headers
          android-grpc-utils
          grpc++
          packet-streamer-client-lib
          packet-streamer-proto-lib
          protobuf::libprotobuf)

add_dependencies(libqemu2-glue virtio_gpu_timelines_fbs_header)

target_compile_definitions(libqemu2-glue INTERFACE "-DCONFIG_VIRGL")
android_add_library(
  TARGET libqemu2-glue-vm-operations LICENSE Apache-2.0
  SRC # cmake-format: sortable
      ${ANDROID_HW_CONFIG_H} qemu-vm-operations-impl.cpp)
target_include_directories(libqemu2-glue-vm-operations
                           PRIVATE .. ../include ${ANDROID_AUTOGEN})
target_link_libraries(
  libqemu2-glue-vm-operations
  PRIVATE android-emu android-emu-base-headers glib2 android-hw-config
          nlohmann_json::nlohmann_json PIXMAN::PIXMAN)

android_add_test(
  TARGET libqemu2-glue_unittests
  SRC base/files/KernelCmdLineLoader_unittest.cpp
      tests/VirtioGpuTimelines_unittest.cpp
      utils/register_crash_info_unittest.cpp)

target_link_libraries(
  libqemu2-glue_unittests
  PRIVATE libqemu2-glue
          qemu-host-common-headers
          android-emu-base
          android-emu-base-headers
          android-emu
          gmock
          gtest
          gmock_main)

target_include_directories(
  libqemu2-glue_unittests
  PRIVATE . ../include ../android/android-emu-base ../android/android-emu .
          ${VIRGLRENDERER_INCLUDE_DIR})
