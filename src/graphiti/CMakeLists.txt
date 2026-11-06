add_library(graphiti INTERFACE)
add_library(Graphiti::graphiti ALIAS graphiti)
target_include_directories(graphiti INTERFACE ${PROJECT_SOURCE_DIR})
target_compile_features(graphiti INTERFACE cxx_std_17)
