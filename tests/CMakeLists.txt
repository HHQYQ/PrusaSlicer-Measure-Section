# TODO Add individual tests as executables in separate directories
# add_subirectory(<testcase>)

find_package(Catch2 3.8 REQUIRED)
slic3r_remap_configs(Catch2::Catch2 RelWithDebInfo Release)
slic3r_remap_configs(Catch2::Catch2WithMain RelWithDebInfo Release)

include(Catch)

set(TEST_DATA_DIR ${CMAKE_CURRENT_SOURCE_DIR}/data)
file(TO_NATIVE_PATH "${TEST_DATA_DIR}" TEST_DATA_DIR)

set(CATCH_EXTRA_ARGS "" CACHE STRING "Extra arguments for catch2 test suites.")

add_library(test_common INTERFACE)
target_include_directories(test_common INTERFACE ${CMAKE_CURRENT_LIST_DIR})
target_compile_definitions(test_common INTERFACE TEST_DATA_DIR=R"\(${TEST_DATA_DIR}\)" CATCH_CONFIG_FAST_COMPILE)
target_link_libraries(test_common INTERFACE Catch2::Catch2WithMain)

if (APPLE)
    target_link_libraries(test_common INTERFACE "-liconv -framework IOKit" "-framework CoreFoundation" -lc++)
endif()

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

add_subdirectory(arrange)
add_subdirectory(thumbnails)
add_subdirectory(libslic3r)
add_subdirectory(fff_print)
add_subdirectory(sla_print)
add_subdirectory(cpp17 EXCLUDE_FROM_ALL)    # does not have to be built all the time

add_executable(mesh_edge_finish_tests libslic3r/test_mesh_edge_finish.cpp)
target_link_libraries(mesh_edge_finish_tests PRIVATE libslic3r)
target_compile_definitions(mesh_edge_finish_tests PRIVATE EDGE_FINISH_TEST_PRINT_CONFIG)
if (WIN32)
    prusaslicer_copy_dlls(mesh_edge_finish_tests)
endif ()
add_test(NAME mesh_edge_finish_tests COMMAND mesh_edge_finish_tests)

add_executable(view_transparency_tests libslic3r/test_view_transparency.cpp)
target_link_libraries(view_transparency_tests PRIVATE libslic3r)
if (WIN32)
    prusaslicer_copy_dlls(view_transparency_tests)
endif ()
add_test(NAME view_transparency_tests COMMAND view_transparency_tests)

add_executable(measure_reference_tests libslic3r/test_measure_references.cpp)
target_link_libraries(measure_reference_tests PRIVATE libslic3r)
if (WIN32)
    prusaslicer_copy_dlls(measure_reference_tests)
endif ()
add_test(NAME measure_reference_tests COMMAND measure_reference_tests)

if (SLIC3R_GUI)
    add_subdirectory(slic3rutils)
endif()


# add_subdirectory(example)
