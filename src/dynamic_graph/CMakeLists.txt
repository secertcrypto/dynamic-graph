add_library(secure_dynamic_graph STATIC engine.cpp)
add_library(DynamicGraph::engine ALIAS secure_dynamic_graph)
target_link_libraries(secure_dynamic_graph PUBLIC RISA::read DynamicGraph::delta_updates Graphiti::graphiti)
add_executable(dynamic_graph_run run.cpp)
target_link_libraries(dynamic_graph_run PRIVATE DynamicGraph::engine nlohmann_json::nlohmann_json)
if(BUILD_TESTING)
    add_executable(dynamic_decode_test test_decode.cpp)
    target_link_libraries(dynamic_decode_test PRIVATE DynamicGraph::engine)
    add_test(NAME dynamic_secure_decode COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/test/run_graphiti_module.py $<TARGET_FILE:dynamic_decode_test> nph 19300)
    add_test(NAME dynamic_graph_iterations COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/check_iterations.py $<TARGET_FILE:dynamic_graph_run>)
    set_tests_properties(dynamic_secure_decode dynamic_graph_iterations PROPERTIES TIMEOUT 150)
endif()
