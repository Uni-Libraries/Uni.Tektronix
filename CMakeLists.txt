#
# Project
#

cmake_minimum_required(VERSION 3.24)

project(uni_tektronix
    VERSION 0.1.0
    LANGUAGES C CXX
)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)
include(GenerateExportHeader)
include(CMakePackageConfigHelpers)



#
# Options
#

option(UNI_TEKTRONIX_BUILD_TESTS "Build Catch2-based unit tests" ON)
option(UNI_TEKTRONIX_BUILD_TOOLS "Build tools" ON)



function(uni_tektronix_apply_warnings target_name)
    if(MSVC)
        target_compile_options(${target_name} PRIVATE /W4)
    else()
        target_compile_options(${target_name} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()



#
# Library
#

add_library(uni_tektronix)

target_sources(uni_tektronix PRIVATE
    src/uni_tektronix_r3f.c
    src/uni_tektronix_r3x.c
)

add_library(uni_tektronix::uni_tektronix ALIAS uni_tektronix)

if(MSVC)
    target_compile_options(uni_tektronix PRIVATE /W4)
else()
    target_compile_options(uni_tektronix PRIVATE -Wall -Wextra -Wpedantic)
endif()

target_compile_features(uni_tektronix PUBLIC c_std_23)


set_target_properties(uni_tektronix PROPERTIES POSITION_INDEPENDENT_CODE ON)

generate_export_header(
    uni_tektronix
    EXPORT_FILE_NAME ${CMAKE_CURRENT_BINARY_DIR}/generated/include/uni_tektronix_export.h
    EXPORT_MACRO_NAME UNI_TEKTRONIX_EXPORT
    NO_EXPORT_MACRO_NAME UNI_TEKTRONIX_NO_EXPORT
    DEPRECATED_MACRO_NAME UNI_TEKTRONIX_DEPRECATED
)

target_include_directories(
    uni_tektronix
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

install(
    TARGETS uni_tektronix
    EXPORT uni_tektronixTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(
    FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/include/uni_tektronix_r3f.h
        ${CMAKE_CURRENT_BINARY_DIR}/generated/include/uni_tektronix_export.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/uni_tektronixConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/uni_tektronixConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/uni_tektronixConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/uni_tektronix
)

install(
    FILES
        ${CMAKE_CURRENT_BINARY_DIR}/uni_tektronixConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/uni_tektronixConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/uni_tektronix
)

install(
    EXPORT uni_tektronixTargets
    FILE uni_tektronixTargets.cmake
    NAMESPACE uni_tektronix::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/uni_tektronix
)



#
# Tools
#

if(UNI_TEKTRONIX_BUILD_TOOLS)
    add_executable(uni_tektronix_tool)

    target_sources(uni_tektronix_tool PRIVATE
        src/uni_tektronix_tool.c
    )

    target_link_libraries(uni_tektronix_tool PRIVATE uni_tektronix $<$<NOT:$<C_COMPILER_ID:MSVC>>:m>)

    if(MSVC)
        target_compile_options(uni_tektronix_tool PRIVATE /W4)
    else()
        target_compile_options(uni_tektronix_tool PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    target_compile_features(uni_tektronix_tool PRIVATE c_std_23)

    install(
        TARGETS uni_tektronix_tool
        EXPORT uni_tektronixTargets
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )
endif()



#
# Tests
#

if(UNI_TEKTRONIX_BUILD_TESTS)
    enable_testing()

    find_package(Catch2 3 CONFIG REQUIRED)

    add_executable(uni_tektronix_tests)

    target_sources(uni_tektronix_tests PRIVATE
        tests/test_r3f.cpp
    )

    target_compile_definitions(
        uni_tektronix_tests
        PRIVATE
            UNI_TEKTRONIX_BUILD_DIR="${CMAKE_CURRENT_BINARY_DIR}"
    )


    if(MSVC)
        target_compile_options(uni_tektronix_tests PRIVATE /W4)
    else()
        target_compile_options(uni_tektronix_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    target_link_libraries(uni_tektronix_tests PRIVATE uni_tektronix Catch2::Catch2WithMain)

    add_test(NAME uni_tektronix_tests COMMAND uni_tektronix_tests)
endif()
