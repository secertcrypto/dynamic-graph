# Both CrIS-MPC and SFSS use the same local EMP target.
add_library(cris_sfssgen STATIC ../sfssgen/dpf.cpp)
target_include_directories(cris_sfssgen PUBLIC
    ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/src/sfssgen/vendor)
target_compile_options(cris_sfssgen PUBLIC -mpclmul)
target_link_libraries(cris_sfssgen PUBLIC emp-tool::emp-tool)
add_library(risa_update STATIC update.cpp)
add_library(RISA::update ALIAS risa_update)
target_link_libraries(risa_update PUBLIC cris_sfssgen)
add_library(dynamic_graph_delta STATIC ../dynamic_graph/delta_updates.cpp)
add_library(DynamicGraph::delta_updates ALIAS dynamic_graph_delta)
target_link_libraries(dynamic_graph_delta PUBLIC RISA::update)
add_executable(risa_update_example example_update.cpp)
target_link_libraries(risa_update_example PRIVATE DynamicGraph::delta_updates)

if(BUILD_TESTING)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    foreach(target risa_update_test risa_batch_test)
        add_executable(${target} ${target}.cpp)
        target_link_libraries(${target} PRIVATE DynamicGraph::delta_updates)
        add_test(NAME ${target} COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/run_two_party.py $<TARGET_FILE:${target}> --port 28465)
        set_tests_properties(${target} PROPERTIES TIMEOUT 120 RESOURCE_LOCK cris_risa_port)
    endforeach()
endif()

add_executable(risa_graphiti_example graphiti_batch_example.cpp)
target_link_libraries(risa_graphiti_example PRIVATE DynamicGraph::delta_updates Graphiti::graphiti)
if(BUILD_TESTING)
    add_test(NAME risa_graphiti_nph COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/test/run_graphiti_module.py
        $<TARGET_FILE:risa_graphiti_example> nph 18500)
    set_tests_properties(risa_graphiti_nph PROPERTIES TIMEOUT 55 RESOURCE_LOCK cris_nph_delta_port)
endif()

option(CRIS_BUILD_RISA_READ "Build Spiral-backed RISA reads" ON)
if(NOT CRIS_BUILD_RISA_READ)
    return()
endif()
add_subdirectory(../spiral ${CMAKE_CURRENT_BINARY_DIR}/spiral)
add_library(risa_read STATIC read.cpp)
add_library(RISA::read ALIAS risa_read)
target_link_libraries(risa_read PUBLIC cris_sfssgen spiral_u64)
add_executable(risa_read_example read_example.cpp)
target_link_libraries(risa_read_example PRIVATE RISA::read)
add_executable(risa_graphiti_read_example graphiti_read_example.cpp)
target_link_libraries(risa_graphiti_read_example PRIVATE RISA::read DynamicGraph::delta_updates Graphiti::graphiti)
if(BUILD_TESTING)
    add_executable(risa_read_test risa_read_test.cpp)
    target_link_libraries(risa_read_test PRIVATE RISA::read)
    add_test(NAME risa_read_two_party COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/run_two_party.py $<TARGET_FILE:risa_read_test> --port 28466)
    add_test(NAME risa_read_graphiti_nph COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/test/run_graphiti_module.py $<TARGET_FILE:risa_graphiti_read_example> nph 18600)
    set_tests_properties(risa_read_two_party risa_read_graphiti_nph PROPERTIES TIMEOUT 120)
endif()

add_subdirectory(../dynamic_graph ${CMAKE_BINARY_DIR}/dynamic_graph)
