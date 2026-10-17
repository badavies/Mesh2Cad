m2c_add_library(io
    SOURCES
        detail/Parse.cpp
        MeshIO.cpp
        Obj.cpp
        Ply.cpp
        Project.cpp
        Stl.cpp
    DEPS
        Mesh2Cad::core
        Mesh2Cad::occt)
target_include_directories(m2c_io PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(m2c_io PRIVATE miniz::miniz nlohmann_json::nlohmann_json)
