#
# Project
#

cmake_minimum_required(VERSION 3.24)

project(uni_tektronix
    VERSION 0.1.0
    LANGUAGES C CXX
)

if(MSVC)
    # Current MSVC support for the C language standard is behind GCC/Clang,
    # so use the highest C standard level that CMake/MSVC reliably expose.
    set(UNI_TEKTRONIX_C_STANDARD 17)
    set(UNI_TEKTRONIX_C_STANDARD_FEATURE c_std_17)
else()
    set(UNI_TEKTRONIX_C_STANDARD 23)
    set(UNI_TEKTRONIX_C_STANDARD_FEATURE c_std_23)
endif()

set(CMAKE_C_STANDARD ${UNI_TEKTRONIX_C_STANDARD})
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
option(
    UNI_TEKTRONIX_ALLOW_CATCH2_AMALGAMATED_DOWNLOAD
    "Download the official Catch2 amalgamated fallback when a system Catch2 package is unavailable"
    ON
)
set(
    UNI_TEKTRONIX_CATCH2_AMALGAMATED_VERSION
    "3.7.1"
    CACHE STRING
    "Pinned Catch2 version used for the amalgamated fallback"
)



function(uni_tektronix_apply_warnings target_name)
    if(MSVC)
        target_compile_options(${target_name} PRIVATE /W4)
    else()
        target_compile_options(${target_name} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

function(uni_tektronix_prepare_catch2_with_main out_target_name)
    if(TARGET Catch2::Catch2WithMain)
        set(${out_target_name} Catch2::Catch2WithMain PARENT_SCOPE)
        return()
    endif()

    find_package(Catch2 3 CONFIG QUIET)
    if(Catch2_FOUND)
        message(STATUS "Using system Catch2 package")
        set(${out_target_name} Catch2::Catch2WithMain PARENT_SCOPE)
        return()
    endif()

    if(NOT UNI_TEKTRONIX_ALLOW_CATCH2_AMALGAMATED_DOWNLOAD)
        message(FATAL_ERROR
            "Catch2 3 was not found. Install a system Catch2 package or enable "
            "UNI_TEKTRONIX_ALLOW_CATCH2_AMALGAMATED_DOWNLOAD to use the official amalgamated fallback."
        )
    endif()

    set(catch2_amalgamated_dir "${CMAKE_CURRENT_BINARY_DIR}/_deps/catch2-amalgamated")
    file(MAKE_DIRECTORY "${catch2_amalgamated_dir}")

    foreach(catch2_file IN ITEMS catch_amalgamated.hpp catch_amalgamated.cpp)
        set(catch2_local_path "${catch2_amalgamated_dir}/${catch2_file}")
        if(NOT EXISTS "${catch2_local_path}")
            set(
                catch2_url
                "https://raw.githubusercontent.com/catchorg/Catch2/v${UNI_TEKTRONIX_CATCH2_AMALGAMATED_VERSION}/extras/${catch2_file}"
            )
            message(STATUS "System Catch2 not found; downloading ${catch2_file} from ${catch2_url}")
            file(DOWNLOAD
                "${catch2_url}"
                "${catch2_local_path}"
                STATUS catch2_download_status
                TLS_VERIFY ON
            )
            list(GET catch2_download_status 0 catch2_download_status_code)
            if(NOT catch2_download_status_code EQUAL 0)
                list(GET catch2_download_status 1 catch2_download_status_message)
                message(FATAL_ERROR
                    "Failed to download ${catch2_file} from ${catch2_url}: ${catch2_download_status_message}"
                )
            endif()
        endif()
    endforeach()

    add_library(uni_tektronix_catch2_with_main STATIC
        "${catch2_amalgamated_dir}/catch_amalgamated.cpp"
    )
    target_compile_features(uni_tektronix_catch2_with_main PUBLIC cxx_std_14)
    target_include_directories(uni_tektronix_catch2_with_main PUBLIC "${catch2_amalgamated_dir}")
    set_target_properties(uni_tektronix_catch2_with_main PROPERTIES POSITION_INDEPENDENT_CODE ON)
    add_library(Catch2::Catch2WithMain ALIAS uni_tektronix_catch2_with_main)

    message(STATUS "Using downloaded Catch2 amalgamated fallback")
    set(${out_target_name} Catch2::Catch2WithMain PARENT_SCOPE)
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

target_compile_features(uni_tektronix PUBLIC ${UNI_TEKTRONIX_C_STANDARD_FEATURE})


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

    target_compile_features(uni_tektronix_tool PRIVATE ${UNI_TEKTRONIX_C_STANDARD_FEATURE})

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

    uni_tektronix_prepare_catch2_with_main(UNI_TEKTRONIX_CATCH2_TARGET)

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

    target_link_libraries(uni_tektronix_tests PRIVATE uni_tektronix ${UNI_TEKTRONIX_CATCH2_TARGET})

    add_test(NAME uni_tektronix_tests COMMAND uni_tektronix_tests)
endif()
