m2c_add_library(rpc
    SOURCES
        RpcServer.cpp
        Session.cpp
    DEPS
        Mesh2Cad::commands
        Mesh2Cad::core)
target_compile_definitions(m2c_rpc PRIVATE MESH2CAD_VERSION="${PROJECT_VERSION}")

add_executable(mesh2cad-rpc main.cpp)
target_link_libraries(mesh2cad-rpc PRIVATE Mesh2Cad::rpc Mesh2Cad::commands m2c_warnings)
target_compile_definitions(mesh2cad-rpc PRIVATE MESH2CAD_VERSION="${PROJECT_VERSION}")
m2c_copy_runtime(mesh2cad-rpc)
