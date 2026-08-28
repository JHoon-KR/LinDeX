cmake_minimum_required(VERSION 3.16)
project(lindex_advc_vaapi C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

set(ADVC_VIDEO_BRIDGE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../video_bridge")
option(ADVC_BUILD_VAAPI_DRIVER "Build the fail-closed ADVC libva vendor driver" ON)
option(ADVC_VAAPI_ENABLE_INPROCESS_REPACK
    "Link Vulkan/Turnip QCOM-to-LINEAR repack into the libva driver" ON)

# The policy API uses the canonical VAProfile type so its unit tests exercise
# the same profile identifiers that applications pass to the driver.  Resolve
# libva before creating that target, not only when the shared driver is added.
find_path(LIBVA_INCLUDE_DIR va/va_backend.h)
if(NOT LIBVA_INCLUDE_DIR)
    message(FATAL_ERROR "libva development headers were not found")
endif()

add_library(advc_protocol_for_vaapi STATIC
    "${ADVC_VIDEO_BRIDGE_DIR}/src/protocol.c"
    "${ADVC_VIDEO_BRIDGE_DIR}/src/capabilities_json.c"
    "${ADVC_VIDEO_BRIDGE_DIR}/src/codec_discovery.c"
    "${ADVC_VIDEO_BRIDGE_DIR}/src/client.c"
    "${ADVC_VIDEO_BRIDGE_DIR}/src/dmabuf_ingress.c")
set_target_properties(advc_protocol_for_vaapi PROPERTIES
    POSITION_INDEPENDENT_CODE ON)
target_include_directories(advc_protocol_for_vaapi PUBLIC
    "${ADVC_VIDEO_BRIDGE_DIR}/include")
target_compile_options(advc_protocol_for_vaapi PRIVATE
    -Wall -Wextra -Wpedantic -Werror)

add_library(advc_vaapi_policy STATIC advc_vaapi_policy.c)
set_target_properties(advc_vaapi_policy PROPERTIES
    POSITION_INDEPENDENT_CODE ON)
target_include_directories(advc_vaapi_policy PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${ADVC_VIDEO_BRIDGE_DIR}/include")
target_include_directories(advc_vaapi_policy SYSTEM PUBLIC
    "${LIBVA_INCLUDE_DIR}")
target_compile_options(advc_vaapi_policy PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(advc_vaapi_policy PUBLIC advc_protocol_for_vaapi)

add_library(advc_vaapi_preflight STATIC advc_vaapi_preflight.c)
set_target_properties(advc_vaapi_preflight PROPERTIES
    POSITION_INDEPENDENT_CODE ON)
target_include_directories(advc_vaapi_preflight PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${ADVC_VIDEO_BRIDGE_DIR}/include")
target_compile_options(advc_vaapi_preflight PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(advc_vaapi_preflight PUBLIC
    advc_vaapi_policy advc_protocol_for_vaapi)

add_executable(advc-vaapi-decode-preflight advc_vaapi_preflight_main.c)
target_link_libraries(advc-vaapi-decode-preflight PRIVATE
    advc_vaapi_preflight advc_protocol_for_vaapi)
target_compile_options(advc-vaapi-decode-preflight PRIVATE
    -Wall -Wextra -Wpedantic -Werror)

add_library(advc_h264_annexb STATIC advc_h264_annexb.c)
set_target_properties(advc_h264_annexb PROPERTIES
    POSITION_INDEPENDENT_CODE ON)
target_include_directories(advc_h264_annexb PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${ADVC_VIDEO_BRIDGE_DIR}/include")
target_compile_options(advc_h264_annexb PRIVATE
    -Wall -Wextra -Wpedantic -Werror)

add_library(advc_vaapi_slice_layout STATIC advc_vaapi_slice_layout.c)
set_target_properties(advc_vaapi_slice_layout PROPERTIES
    POSITION_INDEPENDENT_CODE ON)
target_include_directories(advc_vaapi_slice_layout PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(advc_vaapi_slice_layout PRIVATE
    -Wall -Wextra -Wpedantic -Werror)

add_library(advc_vaapi_surface_policy STATIC advc_vaapi_surface_policy.c)
set_target_properties(advc_vaapi_surface_policy PROPERTIES
    POSITION_INDEPENDENT_CODE ON)
target_include_directories(advc_vaapi_surface_policy PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${ADVC_VIDEO_BRIDGE_DIR}/include")
target_link_libraries(advc_vaapi_surface_policy PUBLIC advc_protocol_for_vaapi)
target_compile_options(advc_vaapi_surface_policy PRIVATE
    -Wall -Wextra -Wpedantic -Werror)

add_library(advc_vaapi_modifier_policy STATIC advc_vaapi_modifier_policy.c)
set_target_properties(advc_vaapi_modifier_policy PROPERTIES
    POSITION_INDEPENDENT_CODE ON)
target_include_directories(advc_vaapi_modifier_policy PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(advc_vaapi_modifier_policy PRIVATE
    -Wall -Wextra -Wpedantic -Werror)

enable_testing()
add_library(advc_vaapi_decode_eos STATIC advc_vaapi_decode_eos.c)
set_target_properties(advc_vaapi_decode_eos PROPERTIES
    POSITION_INDEPENDENT_CODE ON)
target_include_directories(advc_vaapi_decode_eos PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(advc_vaapi_decode_eos PRIVATE
    -Wall -Wextra -Wpedantic -Werror)

add_executable(advc_vaapi_decode_eos_test tests/decode_eos_test.c)
target_link_libraries(advc_vaapi_decode_eos_test PRIVATE
    advc_vaapi_decode_eos)
target_compile_options(advc_vaapi_decode_eos_test PRIVATE
    -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
add_test(NAME advc_vaapi_decode_eos
    COMMAND advc_vaapi_decode_eos_test)

add_executable(advc_vaapi_policy_test tests/policy_test.c)
target_link_libraries(advc_vaapi_policy_test PRIVATE advc_vaapi_policy)
target_compile_options(advc_vaapi_policy_test PRIVATE
    -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
add_test(NAME advc_vaapi_policy COMMAND advc_vaapi_policy_test)

add_executable(advc_vaapi_preflight_test tests/preflight_test.c)
target_link_libraries(advc_vaapi_preflight_test PRIVATE
    advc_vaapi_preflight)
target_compile_options(advc_vaapi_preflight_test PRIVATE
    -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
add_test(NAME advc_vaapi_preflight COMMAND advc_vaapi_preflight_test)

add_executable(advc_h264_annexb_test tests/h264_annexb_test.c)
target_link_libraries(advc_h264_annexb_test PRIVATE advc_h264_annexb)
target_compile_options(advc_h264_annexb_test PRIVATE
    -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
add_test(NAME advc_h264_annexb COMMAND advc_h264_annexb_test)

add_executable(advc_vaapi_slice_layout_test tests/slice_layout_test.c)
target_link_libraries(advc_vaapi_slice_layout_test PRIVATE
    advc_vaapi_slice_layout)
target_compile_options(advc_vaapi_slice_layout_test PRIVATE
    -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
add_test(NAME advc_vaapi_slice_layout COMMAND advc_vaapi_slice_layout_test)

add_executable(advc_vaapi_surface_policy_test tests/surface_policy_test.c)
target_link_libraries(advc_vaapi_surface_policy_test PRIVATE
    advc_vaapi_surface_policy)
target_compile_options(advc_vaapi_surface_policy_test PRIVATE
    -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
add_test(NAME advc_vaapi_surface_policy COMMAND advc_vaapi_surface_policy_test)

add_executable(advc_vaapi_modifier_policy_test tests/modifier_policy_test.c)
target_link_libraries(advc_vaapi_modifier_policy_test PRIVATE
    advc_vaapi_modifier_policy)
target_compile_options(advc_vaapi_modifier_policy_test PRIVATE
    -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
add_test(NAME advc_vaapi_modifier_policy
    COMMAND advc_vaapi_modifier_policy_test)

add_executable(advc_vaapi_capability_probe tests/capability_probe.c)
target_link_libraries(advc_vaapi_capability_probe PRIVATE
    advc_protocol_for_vaapi)
target_compile_options(advc_vaapi_capability_probe PRIVATE
    -Wall -Wextra -Wpedantic -Werror)

if(ADVC_BUILD_VAAPI_DRIVER)
    add_library(advc_vaapi_decode_hevc STATIC
        advc_vaapi_decode_hevc.c)
    set_target_properties(advc_vaapi_decode_hevc PROPERTIES
        POSITION_INDEPENDENT_CODE ON)
    target_include_directories(advc_vaapi_decode_hevc PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${ADVC_VIDEO_BRIDGE_DIR}/include")
    target_link_libraries(advc_vaapi_decode_hevc PUBLIC
        advc_protocol_for_vaapi)
    target_compile_options(advc_vaapi_decode_hevc PRIVATE
        -Wall -Wextra -Wpedantic -Werror)

    add_executable(advc_vaapi_decode_hevc_test
        tests/hevc_decode_test.c)
    target_link_libraries(advc_vaapi_decode_hevc_test PRIVATE
        advc_vaapi_decode_hevc)
    target_compile_options(advc_vaapi_decode_hevc_test PRIVATE
        -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    add_test(NAME advc_vaapi_decode_hevc
        COMMAND advc_vaapi_decode_hevc_test)

    add_library(advc_vaapi_decode_vp9 STATIC
        advc_vaapi_decode_vp9.c)
    set_target_properties(advc_vaapi_decode_vp9 PROPERTIES
        POSITION_INDEPENDENT_CODE ON)
    target_include_directories(advc_vaapi_decode_vp9 PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${ADVC_VIDEO_BRIDGE_DIR}/include")
    target_include_directories(advc_vaapi_decode_vp9 SYSTEM PUBLIC
        "${LIBVA_INCLUDE_DIR}")
    target_link_libraries(advc_vaapi_decode_vp9 PUBLIC
        advc_protocol_for_vaapi)
    target_compile_options(advc_vaapi_decode_vp9 PRIVATE
        -Wall -Wextra -Wpedantic -Werror)

    add_executable(advc_vaapi_decode_vp9_test
        tests/vp9_decode_test.c)
    target_link_libraries(advc_vaapi_decode_vp9_test PRIVATE
        advc_vaapi_decode_vp9)
    target_compile_options(advc_vaapi_decode_vp9_test PRIVATE
        -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    add_test(NAME advc_vaapi_decode_vp9
        COMMAND advc_vaapi_decode_vp9_test)

    add_library(advc_vaapi_encode STATIC advc_vaapi_encode.c)
    set_target_properties(advc_vaapi_encode PROPERTIES
        POSITION_INDEPENDENT_CODE ON)
    target_include_directories(advc_vaapi_encode PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${ADVC_VIDEO_BRIDGE_DIR}/include")
    target_include_directories(advc_vaapi_encode SYSTEM PUBLIC
        "${LIBVA_INCLUDE_DIR}")
    target_link_libraries(advc_vaapi_encode PUBLIC
        advc_protocol_for_vaapi)
    target_compile_options(advc_vaapi_encode PRIVATE
        -Wall -Wextra -Wpedantic -Werror)

    add_executable(advc_vaapi_encode_test tests/encode_test.c)
    target_link_libraries(advc_vaapi_encode_test PRIVATE advc_vaapi_encode)
    target_compile_options(advc_vaapi_encode_test PRIVATE
        -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    add_test(NAME advc_vaapi_encode COMMAND advc_vaapi_encode_test)

    find_package(Threads REQUIRED)
    if(ADVC_VAAPI_ENABLE_INPROCESS_REPACK)
        find_package(Vulkan REQUIRED)
    endif()

    add_library(advc_vaapi_image STATIC advc_vaapi_image.c)
    set_target_properties(advc_vaapi_image PROPERTIES
        POSITION_INDEPENDENT_CODE ON)
    target_include_directories(advc_vaapi_image PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${ADVC_VIDEO_BRIDGE_DIR}/include")
    target_include_directories(advc_vaapi_image SYSTEM PUBLIC
        "${LIBVA_INCLUDE_DIR}")
    target_link_libraries(advc_vaapi_image PUBLIC
        advc_vaapi_encode Threads::Threads)
    target_compile_options(advc_vaapi_image PRIVATE
        -Wall -Wextra -Wpedantic -Werror)

    add_executable(advc_vaapi_image_test tests/image_test.c)
    target_link_libraries(advc_vaapi_image_test PRIVATE advc_vaapi_image)
    target_compile_options(advc_vaapi_image_test PRIVATE
        -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    add_test(NAME advc_vaapi_image COMMAND advc_vaapi_image_test)

    add_library(advc_vaapi_encode_runtime STATIC
        advc_vaapi_encode_runtime.c
        advc_vaapi_encode_eos.c)
    set_target_properties(advc_vaapi_encode_runtime PROPERTIES
        POSITION_INDEPENDENT_CODE ON)
    target_include_directories(advc_vaapi_encode_runtime PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${ADVC_VIDEO_BRIDGE_DIR}/include")
    target_include_directories(advc_vaapi_encode_runtime SYSTEM PUBLIC
        "${LIBVA_INCLUDE_DIR}")
    target_link_libraries(advc_vaapi_encode_runtime PUBLIC
        advc_vaapi_image advc_vaapi_encode advc_protocol_for_vaapi
        advc_vaapi_modifier_policy Threads::Threads)
    target_compile_options(advc_vaapi_encode_runtime PRIVATE
        -Wall -Wextra -Wpedantic -Werror)

    add_executable(advc_vaapi_encode_runtime_test
        tests/encode_runtime_test.c)
    target_link_libraries(advc_vaapi_encode_runtime_test PRIVATE
        advc_vaapi_encode_runtime)
    target_compile_options(advc_vaapi_encode_runtime_test PRIVATE
        -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    add_test(NAME advc_vaapi_encode_runtime
        COMMAND advc_vaapi_encode_runtime_test)

    add_executable(advc_vaapi_encode_eos_test
        tests/encode_eos_test.c)
    target_link_libraries(advc_vaapi_encode_eos_test PRIVATE
        advc_vaapi_encode_runtime)
    target_compile_options(advc_vaapi_encode_eos_test PRIVATE
        -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    add_test(NAME advc_vaapi_encode_eos
        COMMAND advc_vaapi_encode_eos_test)

    set(ADVC_VAAPI_DRIVER_SOURCES
        advc_drv_video.c
        advc_vaapi_decode_runtime.c)
    if(ADVC_VAAPI_ENABLE_INPROCESS_REPACK)
        list(APPEND ADVC_VAAPI_DRIVER_SOURCES
            advc_vaapi_decode_repack_vulkan.c
            "${ADVC_VIDEO_BRIDGE_DIR}/tools/turnip_prime_import.c"
            "${ADVC_VIDEO_BRIDGE_DIR}/tools/turnip_repack_cache.c")
    endif()
    add_library(advc_drv_video SHARED ${ADVC_VAAPI_DRIVER_SOURCES})
    if(ADVC_VAAPI_ENABLE_INPROCESS_REPACK)
        target_compile_definitions(advc_drv_video PRIVATE
            ADVC_VAAPI_INPROCESS_REPACK=1)
    else()
        target_compile_definitions(advc_drv_video PRIVATE
            ADVC_VAAPI_INPROCESS_REPACK=0)
    endif()
    target_include_directories(advc_drv_video PRIVATE
        "${LIBVA_INCLUDE_DIR}"
        "${ADVC_VIDEO_BRIDGE_DIR}/tools")
    target_link_libraries(advc_drv_video PRIVATE
        advc_vaapi_policy advc_h264_annexb advc_vaapi_decode_hevc
        advc_vaapi_decode_vp9 advc_vaapi_slice_layout
        advc_vaapi_surface_policy advc_vaapi_modifier_policy
        advc_vaapi_decode_eos
        advc_vaapi_encode_runtime advc_vaapi_image advc_vaapi_encode
        advc_protocol_for_vaapi
        Threads::Threads)
    if(ADVC_VAAPI_ENABLE_INPROCESS_REPACK)
        target_link_libraries(advc_drv_video PRIVATE Vulkan::Vulkan)
    endif()
    target_compile_options(advc_drv_video PRIVATE
        -Wall -Wextra -Werror)
    set_target_properties(advc_drv_video PROPERTIES
        PREFIX ""
        OUTPUT_NAME "advc_drv_video")

    add_executable(advc_vaapi_decode_repack_vulkan_test
        tests/decode_repack_vulkan_test.c
        advc_vaapi_decode_repack_vulkan.c)
    target_include_directories(advc_vaapi_decode_repack_vulkan_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${ADVC_VIDEO_BRIDGE_DIR}/include"
        "${ADVC_VIDEO_BRIDGE_DIR}/tools")
    target_link_libraries(advc_vaapi_decode_repack_vulkan_test PRIVATE
        advc_protocol_for_vaapi)
    target_compile_options(advc_vaapi_decode_repack_vulkan_test PRIVATE
        -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    add_test(NAME advc_vaapi_decode_repack_vulkan
        COMMAND advc_vaapi_decode_repack_vulkan_test)

    add_executable(advc_vaapi_loader_probe tests/loader_probe.c)
    target_include_directories(advc_vaapi_loader_probe PRIVATE
        "${LIBVA_INCLUDE_DIR}")
    target_link_libraries(advc_vaapi_loader_probe PRIVATE ${CMAKE_DL_LIBS})
    target_compile_options(advc_vaapi_loader_probe PRIVATE
        -Wall -Wextra -Werror)

    add_executable(advc_vaapi_encode_runtime_probe
        tests/encode_runtime_probe.c)
    target_include_directories(advc_vaapi_encode_runtime_probe PRIVATE
        "${LIBVA_INCLUDE_DIR}")
    target_link_libraries(advc_vaapi_encode_runtime_probe PRIVATE
        ${CMAKE_DL_LIBS})
    target_compile_options(advc_vaapi_encode_runtime_probe PRIVATE
        -Wall -Wextra -Werror)

    add_executable(advc_vaapi_decode_runtime_probe
        tests/decode_runtime_probe.c)
    target_include_directories(advc_vaapi_decode_runtime_probe PRIVATE
        "${LIBVA_INCLUDE_DIR}")
    target_link_libraries(advc_vaapi_decode_runtime_probe PRIVATE
        ${CMAKE_DL_LIBS})
    target_compile_options(advc_vaapi_decode_runtime_probe PRIVATE
        -Wall -Wextra -Werror)
endif()
