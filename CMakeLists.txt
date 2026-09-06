cmake_minimum_required(VERSION 3.24)

if(NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
    if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
        set(CMAKE_OSX_DEPLOYMENT_TARGET 15.0 CACHE STRING "Minimum iOS version")
    elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
        set(CMAKE_OSX_DEPLOYMENT_TARGET 10.15 CACHE STRING "Minimum macOS version")
    endif()
endif()

project(rill VERSION 1.0.0 LANGUAGES C CXX)

if(APPLE)
    enable_language(OBJCXX)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^wasm")
    set(ANY_WASM_TOOLCHAIN TRUE)
endif()

set(external "${CMAKE_CURRENT_SOURCE_DIR}/external")
set(CLAP_SDK_ROOT "${external}/clap" CACHE PATH "CLAP SDK")
set(CLAP_HELPERS_ROOT "${external}/clap-helpers" CACHE PATH "clap-helpers")
set(CLAP_WRAPPER_ROOT "${external}/clap-wrapper" CACHE PATH "clap-wrapper")
set(CLAP_WRAPPER_CHOC_ROOT "${external}/choc" CACHE PATH "CHOC")
set(CHAR_CLAP_UTILS_ROOT "${external}/char-clap-utils" CACHE PATH "char-clap-utils")
set(COMPOST_ROOT "${external}/compost" CACHE PATH "compost web components")

foreach(root IN ITEMS CLAP_SDK_ROOT CLAP_HELPERS_ROOT CLAP_WRAPPER_ROOT
                      CLAP_WRAPPER_CHOC_ROOT CHAR_CLAP_UTILS_ROOT COMPOST_ROOT)
    if(NOT EXISTS "${${root}}/CMakeLists.txt" AND NOT EXISTS "${${root}}/package.json"
       AND NOT EXISTS "${${root}}/include/char_clap_utils/WebUI.h")
        message(FATAL_ERROR
            "${root} is empty or missing: ${${root}}\n"
            "Point ${root} at an existing checkout.")
    endif()
endforeach()

if(NOT TARGET clap)
    add_subdirectory("${CLAP_SDK_ROOT}" clap-sdk EXCLUDE_FROM_ALL)
endif()
if(NOT TARGET clap-helpers)
    add_subdirectory("${CLAP_HELPERS_ROOT}" clap-helpers EXCLUDE_FROM_ALL)
endif()
if(NOT TARGET clap-wrapper-extensions)
    add_subdirectory("${CLAP_WRAPPER_ROOT}" clap-wrapper EXCLUDE_FROM_ALL)
endif()

set(PLUGIN_OUTPUT_NAME "Mote")
set(PLUGIN_ID "com.charlieculbert.rill")

include("${CHAR_CLAP_UTILS_ROOT}/cmake/CharClapWebUI.cmake")
char_clap_stage_web_ui(rill
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/ui"
    COMPOST_ROOT "${COMPOST_ROOT}"
    COMPONENTS slider)

add_library(rill-impl STATIC Plugin.cpp)
target_include_directories(rill-impl PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}" "${CHAR_CLAP_UTILS_ROOT}/include")
target_link_libraries(rill-impl PUBLIC clap clap-helpers clap-wrapper-extensions)
char_clap_add_native_webview(rill-impl "${CLAP_WRAPPER_CHOC_ROOT}")
add_custom_target(rill-licenses
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE" "${rill_RESOURCE_DIRECTORY}/LICENSE"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md" "${rill_RESOURCE_DIRECTORY}/THIRD_PARTY_NOTICES.md"
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${rill_RESOURCE_DIRECTORY}/THIRD_PARTY_LICENSES"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_LICENSES" "${rill_RESOURCE_DIRECTORY}/THIRD_PARTY_LICENSES"
    DEPENDS rill-resources
    VERBATIM)
add_dependencies(rill-impl rill-licenses)

if(ANY_WASM_TOOLCHAIN)
    set(formats WCLAP)
else()
    set(formats CLAP)
    if(APPLE AND CMAKE_GENERATOR STREQUAL "Xcode")
        list(APPEND formats AUV3)
    endif()
endif()

make_clapfirst_plugins(
    TARGET_NAME rill
    IMPL_TARGET rill-impl
    OUTPUT_NAME "${PLUGIN_OUTPUT_NAME}"
    ENTRY_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/Entry.cpp"
    RESOURCE_DIRECTORY "${rill_RESOURCE_DIRECTORY}"
    BUNDLE_IDENTIFIER "${PLUGIN_ID}"
    BUNDLE_VERSION "${PROJECT_VERSION}"
    PLUGIN_FORMATS ${formats}
    AUV3_BUILD_STANDALONE TRUE
    AUV2_MANUFACTURER_NAME "Charlie Culbert"
    AUV2_MANUFACTURER_CODE "ChAr"
    AUV2_SUBTYPE_CODE "ChRi"
    AUV2_INSTRUMENT_TYPE "aumi"
    ASSET_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/artifacts"
    COPY_AFTER_BUILD FALSE)

if(TARGET rill_auv3_standalone)
    add_custom_command(TARGET rill_auv3_standalone POST_BUILD
        COMMAND /usr/bin/codesign --force --sign -
            "$<TARGET_BUNDLE_DIR:rill_auv3_standalone>"
        VERBATIM)
endif()

foreach(bundle IN ITEMS rill_clap rill_standalone rill_auv3)
    if(TARGET ${bundle})
        set_property(TARGET ${bundle} APPEND PROPERTY LINK_DEPENDS
            "${CMAKE_CURRENT_BINARY_DIR}/rill-resources.stamp")
    endif()
endforeach()



include(CTest)
if(BUILD_TESTING AND NOT ANY_WASM_TOOLCHAIN AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    add_executable(rill-tests tests/RillTests.cpp)
    target_link_libraries(rill-tests PRIVATE rill-impl)
    add_test(NAME rill-tests COMMAND rill-tests)
endif()
