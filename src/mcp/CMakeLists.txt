# The MCP server is a thin process: it keeps no engine logic and reaches the engine through the
# JSON-RPC layer (in-process when headless, over the app's local socket otherwise).
add_library(m2c_mcp STATIC McpServer.cpp)
target_include_directories(m2c_mcp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(m2c_mcp PUBLIC Mesh2Cad::commands PRIVATE m2c_warnings)
target_compile_definitions(m2c_mcp PRIVATE MESH2CAD_VERSION="${PROJECT_VERSION}")

add_executable(mesh2cad-mcp main.cpp)
target_link_libraries(mesh2cad-mcp PRIVATE m2c_mcp Mesh2Cad::rpc m2c_warnings)
target_compile_definitions(mesh2cad-mcp PRIVATE MESH2CAD_VERSION="${PROJECT_VERSION}")
m2c_copy_runtime(mesh2cad-mcp)
