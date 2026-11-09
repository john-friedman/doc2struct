cmake_minimum_required(VERSION 3.18)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/include/doc2struct/doc2struct.h" DOC2STRUCT_VERSION_LINE REGEX "^#define DOC2STRUCT_VERSION ")
string(REGEX REPLACE ".*\"([^\"]+)\".*" "\\1" DOC2STRUCT_VERSION "${DOC2STRUCT_VERSION_LINE}")
project(doc2struct VERSION ${DOC2STRUCT_VERSION} LANGUAGES C)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
option(DOC2STRUCT_SCALAR_SCAN "Disable custom SIMD scans" OFF)
option(DOC2STRUCT_BUILD_CLI "Build the standalone parsing executable" ON)
option(DOC2STRUCT_BUILD_BENCHMARK "Build the standalone multi-file parsing benchmark" OFF)
string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" PDFIUM_ARCH)
if(PDFIUM_ARCH MATCHES "^(amd64|x86_64)$")
    set(PDFIUM_ARCH "x86_64")
elseif(PDFIUM_ARCH MATCHES "^(aarch64|arm64)$")
    set(PDFIUM_ARCH "arm64")
endif()
if(WIN32)
    if(CMAKE_GENERATOR_PLATFORM MATCHES "ARM64" OR PDFIUM_ARCH STREQUAL "arm64")
        set(PDFIUM_PLATFORM "win_arm64")
    elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(PDFIUM_PLATFORM "win_amd64")
    else()
        set(PDFIUM_PLATFORM "win_x86")
    endif()
    set(PDFIUM_SUFFIX ".dll")
elseif(APPLE)
    set(PDFIUM_PLATFORM "macos_${PDFIUM_ARCH}")
    set(PDFIUM_SUFFIX ".dylib")
else()
    set(PDFIUM_PLATFORM "linux_${PDFIUM_ARCH}")
    set(PDFIUM_SUFFIX ".so")
endif()
set(PDFIUM_BUNDLE "${CMAKE_CURRENT_SOURCE_DIR}/vendor/pdfium/runtime/${PDFIUM_PLATFORM}")
set(PDFIUM_LIBRARY "${PDFIUM_BUNDLE}/doc2struct_pdfium${PDFIUM_SUFFIX}" CACHE FILEPATH "Required doc2struct PDFium bridge library (not an import library)")
get_filename_component(PDFIUM_DIRECTORY "${PDFIUM_LIBRARY}" DIRECTORY)
set(PDFIUM_LICENSE_DIR "${PDFIUM_DIRECTORY}/licenses" CACHE PATH "Redistribution notices accompanying this PDFium binary (required for install)")
if(NOT EXISTS "${PDFIUM_LIBRARY}" OR IS_DIRECTORY "${PDFIUM_LIBRARY}")
    message(FATAL_ERROR "The doc2struct PDFium bridge is required. Build build_scripts/build_pdfium.py --fetch, or set PDFIUM_LIBRARY and PDFIUM_LICENSE_DIR to a custom runtime bundle.")
endif()
set(PDFIUM_RUNTIME "doc2struct_pdfium${PDFIUM_SUFFIX}")
configure_file("${PDFIUM_LIBRARY}" "${CMAKE_CURRENT_BINARY_DIR}/${PDFIUM_RUNTIME}" COPYONLY)
# Generated data is checked in: Python is optional for native consumers.
find_package(Python3 QUIET COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../presets/defaults.json")
    file(GLOB PRESET_INPUTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../presets/*.json")
    add_custom_command(OUTPUT "${CMAKE_CURRENT_SOURCE_DIR}/src/preset_data.h"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../build_scripts/generate_presets.py"
        DEPENDS ${PRESET_INPUTS} "${CMAKE_CURRENT_SOURCE_DIR}/../build_scripts/generate_presets.py"
        VERBATIM)
endif()
add_library(doc2struct STATIC
    src/preset_data.h
    src/api.c
    src/html_pipeline.c
    src/pdf.c
    src/pdf_layout.c
    src/config.c
    src/config_json.c
    src/memory.c
    src/text.c
    src/text_layout.c
    src/rules.c
    src/standardized_text.c
    src/lists.c
    src/json.c
    src/markdown.c
    src/tables.c
    vendor/pcre2/pcre2_auto_possess.c
    vendor/pcre2/pcre2_chkdint.c
    vendor/pcre2/pcre2_compile.c
    vendor/pcre2/pcre2_compile_cgroup.c
    vendor/pcre2/pcre2_compile_class.c
    vendor/pcre2/pcre2_config.c
    vendor/pcre2/pcre2_context.c
    vendor/pcre2/pcre2_convert.c
    vendor/pcre2/pcre2_dfa_match.c
    vendor/pcre2/pcre2_error.c
    vendor/pcre2/pcre2_extuni.c
    vendor/pcre2/pcre2_find_bracket.c
    vendor/pcre2/pcre2_jit_compile.c
    vendor/pcre2/pcre2_maketables.c
    vendor/pcre2/pcre2_match.c
    vendor/pcre2/pcre2_match_data.c
    vendor/pcre2/pcre2_match_next.c
    vendor/pcre2/pcre2_newline.c
    vendor/pcre2/pcre2_ord2utf.c
    vendor/pcre2/pcre2_pattern_info.c
    vendor/pcre2/pcre2_script_run.c
    vendor/pcre2/pcre2_serialize.c
    vendor/pcre2/pcre2_string_utils.c
    vendor/pcre2/pcre2_study.c
    vendor/pcre2/pcre2_substitute.c
    vendor/pcre2/pcre2_substring.c
    vendor/pcre2/pcre2_tables.c
    vendor/pcre2/pcre2_ucd.c
    vendor/pcre2/pcre2_valid_utf.c
    vendor/pcre2/pcre2_xclass.c
    vendor/pcre2/pcre2_chartables.c
)
add_library(doc2struct::doc2struct ALIAS doc2struct)
set_target_properties(doc2struct PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES POSITION_INDEPENDENT_CODE YES)
target_compile_features(doc2struct PUBLIC c_std_11)
target_include_directories(doc2struct PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}> PRIVATE src vendor/pcre2 vendor/pdfium)
target_compile_definitions(doc2struct PRIVATE HAVE_CONFIG_H=1 PCRE2_STATIC=1 SUPPORT_PCRE2_8=1 SUPPORT_UNICODE=1 HAVE_MEMMOVE=1 HAVE_STDLIB_H=1 HAVE_STDINT_H=1 HAVE_STRING_H=1 HAVE_INTTYPES_H=1 HAVE_LIMITS_H=1 PCRE2_CODE_UNIT_WIDTH=8)
if(DOC2STRUCT_SCALAR_SCAN)
    target_compile_definitions(doc2struct PRIVATE DS_SCALAR_SCAN=1)
endif()
if(MSVC)
    target_compile_options(doc2struct PRIVATE /utf-8)
    target_compile_definitions(doc2struct PRIVATE _CRT_SECURE_NO_WARNINGS=1)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(doc2struct PUBLIC m Threads::Threads ${CMAKE_DL_LIBS})
endif()
if(DOC2STRUCT_BUILD_CLI)
    add_executable(doc2struct_parse src/cli.c)
    target_link_libraries(doc2struct_parse PRIVATE doc2struct)
    target_compile_features(doc2struct_parse PRIVATE c_std_11)
    add_custom_command(TARGET doc2struct_parse POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PDFIUM_LIBRARY}" "$<TARGET_FILE_DIR:doc2struct_parse>/${PDFIUM_RUNTIME}")
    install(TARGETS doc2struct_parse RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT cli)
endif()
if(MSVC)
    # A native archive must run on machines without Visual Studio or Python.
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION "${CMAKE_INSTALL_BINDIR}")
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_COMPONENT cli)
    include(InstallRequiredSystemLibraries)
endif()
if(DOC2STRUCT_BUILD_BENCHMARK)
    add_executable(doc2struct_text_bench bench/text_many.c)
    target_link_libraries(doc2struct_text_bench PRIVATE doc2struct)
    target_compile_features(doc2struct_text_bench PRIVATE c_std_11)
    add_executable(doc2struct_bench bench/test_many.c)
    target_link_libraries(doc2struct_bench PRIVATE doc2struct)
    target_compile_features(doc2struct_bench PRIVATE c_std_11)
    if(MINGW)
        target_link_options(doc2struct_bench PRIVATE -municode)
    endif()
endif()
install(TARGETS doc2struct EXPORT doc2structTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT sdk)
if(NOT IS_DIRECTORY "${PDFIUM_LICENSE_DIR}")
    message(FATAL_ERROR "Set PDFIUM_LICENSE_DIR to the notices accompanying PDFIUM_LIBRARY before installing.")
endif()
if(NOT EXISTS "${PDFIUM_DIRECTORY}/version.json")
    message(FATAL_ERROR "PDFium version.json must accompany PDFIUM_LIBRARY.")
endif()
foreach(component sdk cli)
    install(FILES "${PDFIUM_LIBRARY}" DESTINATION ${CMAKE_INSTALL_BINDIR} RENAME "${PDFIUM_RUNTIME}" COMPONENT ${component})
    install(FILES "${PDFIUM_DIRECTORY}/version.json" DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT ${component})
    install(DIRECTORY "${PDFIUM_LICENSE_DIR}/" DESTINATION ${CMAKE_INSTALL_DATADIR}/doc2struct/licenses/pdfium COMPONENT ${component})
    install(FILES vendor/pdfium/LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/doc2struct/licenses/pdfium COMPONENT ${component})
    install(FILES vendor/pcre2/LICENCE.md DESTINATION ${CMAKE_INSTALL_DATADIR}/doc2struct/licenses/pcre2 COMPONENT ${component})
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../LICENSE" DESTINATION ${CMAKE_INSTALL_DATADIR}/doc2struct COMPONENT ${component})
endforeach()
install(DIRECTORY include/doc2struct DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} COMPONENT sdk)
install(EXPORT doc2structTargets NAMESPACE doc2struct:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/doc2struct COMPONENT sdk)
configure_package_config_file("${CMAKE_CURRENT_SOURCE_DIR}/doc2structConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/doc2structConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/doc2struct
    PATH_VARS CMAKE_INSTALL_BINDIR)
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/doc2structConfigVersion.cmake" VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/doc2structConfig.cmake" "${CMAKE_CURRENT_BINARY_DIR}/doc2structConfigVersion.cmake" DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/doc2struct COMPONENT sdk)
