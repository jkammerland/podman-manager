cmake_minimum_required(VERSION 3.31)

project(
    pod_installer
    VERSION 0.1.0
    DESCRIPTION "MVP root orchestrator library for rootless Podman users"
    LANGUAGES CXX)

option(POD_INSTALLER_BUILD_EXAMPLES "Build example services" ON)
option(POD_INSTALLER_BUILD_TESTS "Build tests" ON)
option(POD_INSTALLER_WITH_SDBUS "Build the sdbus-c++ systemd backend when available" ON)
set(POD_INSTALLER_GENTEST_SOURCE_DIR "" CACHE PATH
    "Local gentest source checkout override for offline builds")

if(POD_INSTALLER_BUILD_TESTS)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL
        "Export compile commands for gentest code generation" FORCE)
endif()

find_package(CURL REQUIRED)
find_package(Threads REQUIRED)
find_package(PkgConfig QUIET)

if(POD_INSTALLER_WITH_SDBUS AND PkgConfig_FOUND)
    pkg_check_modules(SDBUSCPP QUIET IMPORTED_TARGET sdbus-c++)
endif()

add_library(
    pod_installer
    src/deployment.cpp
    src/container_spec.cpp
    src/error.cpp
    src/http.cpp
    src/podman_client.cpp
    src/quadlet.cpp
    src/socket_validation.cpp
    src/systemd.cpp
    src/target.cpp)

if(SDBUSCPP_FOUND)
    target_sources(pod_installer PRIVATE src/systemd_sdbus.cpp)
endif()

add_library(pod_installer::pod_installer ALIAS pod_installer)

target_compile_features(pod_installer PUBLIC cxx_std_23)
target_include_directories(
    pod_installer
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)
target_link_libraries(pod_installer PUBLIC CURL::libcurl Threads::Threads)

if(SDBUSCPP_FOUND)
    target_link_libraries(pod_installer PUBLIC PkgConfig::SDBUSCPP)
    target_compile_definitions(pod_installer PUBLIC POD_INSTALLER_HAS_SDBUS=1)
else()
    target_compile_definitions(pod_installer PUBLIC POD_INSTALLER_HAS_SDBUS=0)
endif()

target_compile_options(
    pod_installer
    PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic>)

if(POD_INSTALLER_BUILD_EXAMPLES)
    add_executable(pod_installer_example_service examples/orchestrator_service.cpp)
    target_compile_features(pod_installer_example_service PRIVATE cxx_std_23)
    target_link_libraries(pod_installer_example_service PRIVATE pod_installer::pod_installer)
    target_compile_options(
        pod_installer_example_service
        PRIVATE
            $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic>)
endif()

if(POD_INSTALLER_BUILD_TESTS)
    get_property(is_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
    if(is_multi_config)
        message(FATAL_ERROR
            "POD_INSTALLER_BUILD_TESTS requires a single-config generator because "
            "gentest code generation consumes compile_commands.json. Use Ninja/Unix "
            "Makefiles or configure with -DPOD_INSTALLER_BUILD_TESTS=OFF.")
    endif()

    enable_testing()

    find_package(gentest CONFIG QUIET)
    if(NOT TARGET gentest::gentest_main)
        include(FetchContent)
        set(gentest_BUILD_TESTING OFF CACHE BOOL "Do not build gentest's own test suite" FORCE)
        set(GENTEST_BUILD_CODEGEN ON CACHE BOOL "Build gentest_codegen for pod_installer tests" FORCE)

        if(POD_INSTALLER_GENTEST_SOURCE_DIR)
            FetchContent_Declare(gentest SOURCE_DIR "${POD_INSTALLER_GENTEST_SOURCE_DIR}")
        else()
            FetchContent_Declare(
                gentest
                GIT_REPOSITORY https://github.com/jkammerland/gentest.git
                GIT_TAG 189a02de1932723671c4a103509d26f027bb8fc7)
        endif()
        FetchContent_MakeAvailable(gentest)
    endif()

    add_executable(pod_installer_tests tests/pod_installer_tests.cpp)
    target_compile_features(pod_installer_tests PRIVATE cxx_std_23)
    target_link_libraries(pod_installer_tests PRIVATE pod_installer::pod_installer gentest::gentest_main)
    target_compile_options(
        pod_installer_tests
        PRIVATE
            $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic -Wno-attributes -Wno-unused-function -Wno-unused-private-field>
            $<$<CXX_COMPILER_ID:MSVC>:/wd5030>)

    gentest_attach_codegen(pod_installer_tests)
    gentest_discover_tests(pod_installer_tests)
endif()
