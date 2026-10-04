cmake_minimum_required(VERSION 3.16)
project(libecsdo VERSION 2.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

set(IGH_SOURCE_DIR "" CACHE PATH
    "Configured IgH EtherCAT Master source tree (contains master/ioctl.h)")
if(NOT IGH_SOURCE_DIR)
    message(FATAL_ERROR "Set -DIGH_SOURCE_DIR=<configured IgH source tree>")
endif()
if(NOT EXISTS "${IGH_SOURCE_DIR}/master/ioctl.h")
    message(FATAL_ERROR "${IGH_SOURCE_DIR}/master/ioctl.h not found")
endif()
if(NOT EXISTS "${IGH_SOURCE_DIR}/config.h")
    message(FATAL_ERROR
        "${IGH_SOURCE_DIR}/config.h not found (run ./configure in the IgH tree)")
endif()

option(ECSDO_BUILD_EXAMPLES "Build examples/ecsdo_cli" ON)
option(ECSDO_BUILD_TESTS "Build unit tests" ON)
option(ECSDO_BUILD_OP_TEST "Build examples/op_test (needs libethercat)" ON)

# Library logic without the ioctl layer (tests link a fake one instead).
add_library(ecsdo_core OBJECT src/ecsdo.c src/ecsdo_types.c)
target_include_directories(ecsdo_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(ecsdo_core PRIVATE -Wall -Wextra)
set_target_properties(ecsdo_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(ecsdo src/ecsdo_ioctl.c $<TARGET_OBJECTS:ecsdo_core>)
target_include_directories(ecsdo
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
# IgH headers only for the ioctl layer.
set_source_files_properties(src/ecsdo_ioctl.c PROPERTIES
    INCLUDE_DIRECTORIES
        "${IGH_SOURCE_DIR};${IGH_SOURCE_DIR}/master;${IGH_SOURCE_DIR}/include")
target_compile_options(ecsdo PRIVATE -Wall -Wextra)
set_target_properties(ecsdo PROPERTIES
    VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR}
    POSITION_INDEPENDENT_CODE ON)

# Optional helpers (JSON dump, values by name), only public API inside.
add_library(ecsdo_util util/ecsdo_util.c)
target_link_libraries(ecsdo_util PUBLIC ecsdo)
target_compile_options(ecsdo_util PRIVATE -Wall -Wextra)
set_target_properties(ecsdo_util PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(ECSDO_BUILD_EXAMPLES)
    add_executable(ecsdo_cli examples/ecsdo_cli.c)
    target_link_libraries(ecsdo_cli PRIVATE ecsdo_util ecsdo)
    target_compile_options(ecsdo_cli PRIVATE -Wall -Wextra)
    add_executable(startup_check examples/startup_check.c)
    target_link_libraries(startup_check PRIVATE ecsdo_util ecsdo)
    target_compile_options(startup_check PRIVATE -Wall -Wextra)
endif()

if(ECSDO_BUILD_OP_TEST)
    find_path(ECRT_INCLUDE_DIR ecrt.h HINTS /usr/local/include)
    find_library(ETHERCAT_LIBRARY ethercat HINTS /usr/local/lib)
    if(ECRT_INCLUDE_DIR AND ETHERCAT_LIBRARY)
        add_subdirectory(examples/op_test)
    else()
        message(STATUS "libethercat not found, skipping examples/op_test")
    endif()
endif()

if(ECSDO_BUILD_TESTS)
    find_program(MEMORYCHECK_COMMAND valgrind)
    set(MEMORYCHECK_COMMAND_OPTIONS
        "--leak-check=full --errors-for-leak-kinds=all --error-exitcode=1")
    include(CTest)
    add_executable(test_ecsdo tests/test_ecsdo.c tests/fake_ioctl.c
        $<TARGET_OBJECTS:ecsdo_core>)
    target_include_directories(test_ecsdo PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_compile_options(test_ecsdo PRIVATE -Wall -Wextra)
    add_test(NAME ecsdo COMMAND test_ecsdo)

    add_executable(test_items tests/test_items.c util/ecsdo_util.c
        tests/fake_ioctl.c $<TARGET_OBJECTS:ecsdo_core>)
    target_include_directories(test_items PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_compile_options(test_items PRIVATE -Wall -Wextra)
    add_test(NAME items COMMAND test_items)
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

target_include_directories(ecsdo_util PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

install(TARGETS ecsdo ecsdo_util EXPORT ecsdoTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES include/ecsdo/ecsdo.h include/ecsdo/ecsdo_util.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/ecsdo)
if(ECSDO_BUILD_EXAMPLES)
    install(TARGETS ecsdo_cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# find_package(ecsdo) -> ecsdo::ecsdo, ecsdo::ecsdo_util
install(EXPORT ecsdoTargets NAMESPACE ecsdo::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ecsdo)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/ecsdoConfig.cmake.in
    "@PACKAGE_INIT@\ninclude(\"\${CMAKE_CURRENT_LIST_DIR}/ecsdoTargets.cmake\")\n")
configure_package_config_file(${CMAKE_CURRENT_BINARY_DIR}/ecsdoConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/ecsdoConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ecsdo)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/ecsdoConfigVersion.cmake
    COMPATIBILITY SameMajorVersion)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/ecsdoConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/ecsdoConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ecsdo)
