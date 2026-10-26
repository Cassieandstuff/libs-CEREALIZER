# ---------------------------------------------------------------------------
# sct-serial — reflect()-driven serialization with a pluggable Backend, plus a
# compact validated binary file format. Header-only, pure std, no dependencies.
#
# Standalone-ready: builds on its own (`cmake -S . -B build`) or as a
# subdirectory of a larger build. Consumers link the alias `sct::serial` and
# include <sct-serial/SctSerial.h> / <sct-serial/SctSerialFile.h>. Installed, it
# exports a CMake package: find_package(sct-serial CONFIG REQUIRED).
# ---------------------------------------------------------------------------

cmake_minimum_required(VERSION 3.25)

project(sct-serial
    VERSION     0.1.0
    DESCRIPTION "reflect()-driven serialization with a pluggable backend"
    LANGUAGES   CXX
)

option(SCT_SERIAL_BUILD_TESTS "Build the sct-serial unit test" ${PROJECT_IS_TOP_LEVEL})
option(SCT_SERIAL_INSTALL     "Generate sct-serial's install rules" ${PROJECT_IS_TOP_LEVEL})

include(GNUInstallDirs)

add_library(sct-serial INTERFACE)
add_library(sct::serial ALIAS sct-serial)
set_target_properties(sct-serial PROPERTIES EXPORT_NAME serial)
target_include_directories(sct-serial INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
target_compile_features(sct-serial INTERFACE cxx_std_23)

# ── Tests ────────────────────────────────────────────────────────────────────
if(SCT_SERIAL_BUILD_TESTS)
    enable_testing()
    add_executable(sct-serial-tests "${CMAKE_CURRENT_SOURCE_DIR}/tests/SerialTest.cpp")
    target_link_libraries(sct-serial-tests PRIVATE sct::serial)
    if(MSVC)
        target_compile_options(sct-serial-tests PRIVATE /W4 /MP)
    endif()
    add_test(NAME sct-serial-tests COMMAND sct-serial-tests)
endif()

# ── Install + package ────────────────────────────────────────────────────────
if(SCT_SERIAL_INSTALL)
    include(CMakePackageConfigHelpers)
    set(_pkgdir "${CMAKE_INSTALL_DATADIR}/sct-serial")

    install(TARGETS sct-serial EXPORT sct-serialTargets)
    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
    install(EXPORT sct-serialTargets NAMESPACE sct:: DESTINATION "${_pkgdir}")

    configure_package_config_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/sct-serialConfig.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/sct-serialConfig.cmake"
        INSTALL_DESTINATION "${_pkgdir}")
    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/sct-serialConfigVersion.cmake"
        COMPATIBILITY SameMinorVersion
        ARCH_INDEPENDENT)
    install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/sct-serialConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/sct-serialConfigVersion.cmake"
        DESTINATION "${_pkgdir}")
endif()
