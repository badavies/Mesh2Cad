add_executable(mesh2cad main.cpp)
target_link_libraries(mesh2cad PRIVATE Mesh2Cad::commands Mesh2Cad::document Mesh2Cad::core m2c_warnings)
target_compile_definitions(mesh2cad PRIVATE MESH2CAD_VERSION="${PROJECT_VERSION}")
m2c_copy_runtime(mesh2cad)
