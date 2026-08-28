cmake_minimum_required(VERSION 3.16)
project(android_debian_video_codec_bridge C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

add_library(advc_protocol STATIC
    src/protocol.c
    src/capabilities_json.c
    src/codec_discovery.c
    src/broker.c
    src/client.c
    src/glibc_import.c
    src/ahb_prime_mapper.c
    src/dmabuf_ingress.c
    src/session_engine.c
    src/transport_policy.c
    src/byte_range.c)
target_include_directories(advc_protocol PUBLIC include)
target_compile_options(advc_protocol PRIVATE -Wall -Wextra -Wpedantic -Werror)

if(ANDROID)
    add_library(advc_encode_surface STATIC
        android/encode_surface_core.c
        android/encode_surface_ndk.c
        android/encode_surface_egl.c
        android/encode_surface_vulkan.c)
    target_include_directories(advc_encode_surface PUBLIC android)
    target_link_libraries(advc_encode_surface PUBLIC
        advc_protocol android mediandk EGL GLESv2 vulkan dl)
    target_compile_options(advc_encode_surface PRIVATE
        -Wall -Wextra -Wpedantic -Werror)

    add_library(advc_android_probe STATIC
        android/android_capability_probe.c
        android/surface_encode_probe.c
        android/surface_ahb_probe.c
        android/surface_dmabuf_probe.c
        android/android_ahb_socket_probe.c
        android/android_prime_mapper.c
        android/stable_mapper_metadata.c
        android/binder_pool.c)
    target_include_directories(advc_android_probe PUBLIC include android)
    target_link_libraries(advc_android_probe PUBLIC
        advc_protocol advc_encode_surface android mediandk dl)
    target_compile_options(advc_android_probe PRIVATE -Wall -Wextra -Werror)

    add_executable(advc-capability-probe android/capability_probe_main.c
        android/android_codec_backend.c android/android_ahb_decode.c)
    target_link_libraries(advc-capability-probe PRIVATE advc_android_probe)
    target_compile_options(advc-capability-probe PRIVATE -Wall -Wextra -Wpedantic -Werror)

    add_executable(advc-broker android/broker_main.c android/android_codec_backend.c
        android/android_ahb_decode.c)
    target_include_directories(advc-broker PRIVATE android)
    target_link_libraries(advc-broker PRIVATE advc_android_probe)
    target_compile_options(advc-broker PRIVATE -Wall -Wextra -Wpedantic -Werror)

    add_executable(advc-decode-smoke-android
        tools/advc_decode_smoke.c tools/advc_annexb.c
        android/android_prime_mapper.c android/stable_mapper_metadata.c)
    target_include_directories(advc-decode-smoke-android PRIVATE tools android)
    target_link_libraries(advc-decode-smoke-android PRIVATE
        advc_protocol android mediandk dl)
    target_compile_options(advc-decode-smoke-android PRIVATE
        -Wall -Wextra -Wpedantic -Werror)

    add_executable(advc-encode-smoke-android
        tools/advc_encode_smoke.c tools/advc_bitstream.c)
    target_include_directories(advc-encode-smoke-android PRIVATE tools android)
    target_link_libraries(advc-encode-smoke-android PRIVATE advc_protocol android)
    target_compile_options(advc-encode-smoke-android PRIVATE
        -Wall -Wextra -Wpedantic -Werror)

    add_executable(advc-pair-smoke-android tools/advc_pair_smoke_android.c)
    target_compile_options(advc-pair-smoke-android PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
endif()

if(NOT ANDROID)
    enable_testing()
    find_package(Threads REQUIRED)
    add_executable(protocol_test tests/protocol_test.c)
    target_link_libraries(protocol_test PRIVATE advc_protocol)
    target_compile_options(protocol_test PRIVATE
        -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    add_test(NAME protocol_test COMMAND protocol_test)

    add_executable(codec_discovery_test tests/codec_discovery_test.c)
    target_link_libraries(codec_discovery_test PRIVATE advc_protocol)
    target_compile_options(codec_discovery_test PRIVATE
        -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    add_test(NAME codec_discovery_test COMMAND codec_discovery_test)

    add_executable(session_engine_test tests/session_engine_test.c)
    target_link_libraries(session_engine_test PRIVATE advc_protocol)
    target_compile_options(session_engine_test PRIVATE
        -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    add_test(NAME session_engine_test COMMAND session_engine_test)

    add_executable(client_test tests/client_test.c tools/advc_annexb.c)
    target_include_directories(client_test PRIVATE tools)
    target_link_libraries(client_test PRIVATE advc_protocol)
    target_compile_options(client_test PRIVATE
        -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    add_test(NAME client_test COMMAND client_test)

    add_executable(bitstream_test tests/bitstream_test.c tools/advc_bitstream.c)
    target_include_directories(bitstream_test PRIVATE tools)
    target_compile_options(bitstream_test PRIVATE
        -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    add_test(NAME bitstream_test COMMAND bitstream_test)

    add_executable(android_codec_backend_test
        tests/android_codec_backend_test.c
        android/android_codec_backend.c
        tests/android_ahb_decode_stub.c
        tests/encode_surface_backend_stub.c
        src/byte_range.c)
    target_include_directories(android_codec_backend_test PRIVATE
        tests/android_stubs android include)
    target_compile_options(android_codec_backend_test PRIVATE
        -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    add_test(NAME android_codec_backend_test COMMAND android_codec_backend_test)

    add_executable(encode_surface_test
        tests/encode_surface_test.c
        android/encode_surface_core.c)
    target_include_directories(encode_surface_test PRIVATE android)
    target_compile_options(encode_surface_test PRIVATE
        -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    add_test(NAME encode_surface_test COMMAND encode_surface_test)

    add_executable(glibc_import_test tests/glibc_import_test.c)
    target_link_libraries(glibc_import_test PRIVATE advc_protocol)
    target_compile_options(glibc_import_test PRIVATE
        -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    add_test(NAME glibc_import_test COMMAND glibc_import_test)

    add_executable(dmabuf_ingress_test tests/dmabuf_ingress_test.c)
    target_link_libraries(dmabuf_ingress_test PRIVATE advc_protocol)
    target_compile_options(dmabuf_ingress_test PRIVATE
        -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    add_test(NAME dmabuf_ingress_test COMMAND dmabuf_ingress_test)

    add_executable(ahb_prime_mapper_test tests/ahb_prime_mapper_test.c)
    target_link_libraries(ahb_prime_mapper_test PRIVATE advc_protocol)
    target_compile_options(ahb_prime_mapper_test PRIVATE
        -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    add_test(NAME ahb_prime_mapper_test COMMAND ahb_prime_mapper_test)

    add_executable(transport_policy_test tests/transport_policy_test.c)
    target_link_libraries(transport_policy_test PRIVATE advc_protocol)
    target_compile_options(transport_policy_test PRIVATE
        -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    add_test(NAME transport_policy_test COMMAND transport_policy_test)

    add_executable(stable_mapper_metadata_test
        tests/stable_mapper_metadata_test.c android/stable_mapper_metadata.c)
    target_include_directories(stable_mapper_metadata_test PRIVATE android include)
    target_compile_options(stable_mapper_metadata_test PRIVATE
        -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    add_test(NAME stable_mapper_metadata_test COMMAND stable_mapper_metadata_test)

    add_executable(turnip_repack_cache_test
        tests/turnip_repack_cache_test.c tools/turnip_repack_cache.c)
    target_include_directories(turnip_repack_cache_test PRIVATE tools include)
    target_compile_options(turnip_repack_cache_test PRIVATE
        -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    add_test(NAME turnip_repack_cache_test COMMAND turnip_repack_cache_test)

    add_executable(firefox_rdd_socket_test
        tests/firefox_rdd_socket_test.c)
    target_link_libraries(firefox_rdd_socket_test PRIVATE
        Threads::Threads ${CMAKE_DL_LIBS})
    target_compile_options(firefox_rdd_socket_test PRIVATE
        -Wall -Wextra -Werror -UNDEBUG)
    add_test(NAME firefox_rdd_socket_test COMMAND firefox_rdd_socket_test)

    add_executable(advc-decode-smoke tools/advc_decode_smoke.c tools/advc_annexb.c)
    target_include_directories(advc-decode-smoke PRIVATE tools)
    target_link_libraries(advc-decode-smoke PRIVATE advc_protocol)
    target_compile_options(advc-decode-smoke PRIVATE -Wall -Wextra -Wpedantic -Werror)

    find_package(Vulkan QUIET)
    if(Vulkan_FOUND)
        # Validation-only; the final module carries no smoke/probe executable.
        add_executable(advc-decode-turnip-smoke
            tools/advc_decode_smoke.c tools/advc_annexb.c
            tools/turnip_prime_import.c tools/turnip_repack_cache.c)
        target_include_directories(advc-decode-turnip-smoke PRIVATE tools)
        target_compile_definitions(advc-decode-turnip-smoke PRIVATE
            ADVC_TURNIP_VALIDATE=1)
        target_link_libraries(advc-decode-turnip-smoke PRIVATE
            advc_protocol Vulkan::Vulkan Threads::Threads)
        target_compile_options(advc-decode-turnip-smoke PRIVATE
            -Wall -Wextra -Wpedantic -Werror)

        add_executable(advc-repack-gateway
            tools/advc_repack_gateway.c tools/turnip_prime_import.c
            tools/turnip_repack_cache.c)
        target_include_directories(advc-repack-gateway PRIVATE tools)
        target_link_libraries(advc-repack-gateway PRIVATE
            advc_protocol Vulkan::Vulkan Threads::Threads)
        target_compile_options(advc-repack-gateway PRIVATE
            -Wall -Wextra -Wpedantic -Werror)

        add_executable(repack_gateway_lifecycle_test
            tests/repack_gateway_lifecycle_test.c
            tools/turnip_prime_import.c tools/turnip_repack_cache.c)
        target_include_directories(repack_gateway_lifecycle_test PRIVATE tools)
        target_link_libraries(repack_gateway_lifecycle_test PRIVATE
            advc_protocol Vulkan::Vulkan Threads::Threads)
        target_compile_options(repack_gateway_lifecycle_test PRIVATE
            -Wall -Wextra -Wpedantic -Werror -Wno-unused-function
            -Wno-unused-variable -UNDEBUG)
        add_test(NAME repack_gateway_lifecycle_test
            COMMAND repack_gateway_lifecycle_test)
    endif()

    add_executable(advc-encode-smoke
        tools/advc_encode_smoke.c tools/advc_bitstream.c)
    target_include_directories(advc-encode-smoke PRIVATE tools)
    target_link_libraries(advc-encode-smoke PRIVATE advc_protocol)
    target_compile_options(advc-encode-smoke PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_executable(advc-ctl tools/advc_ctl.c)
target_link_libraries(advc-ctl PRIVATE advc_protocol)
target_compile_options(advc-ctl PRIVATE -Wall -Wextra -Wpedantic -Werror)
