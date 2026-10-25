# Desktop app (ADR 0023). Built only when Qt 6 is found; see README "Building the app".
set(CMAKE_AUTOMOC ON)

# Everything but main(), so tests/app can drive the real window.
add_library(m2c_app STATIC
    Engine.cpp
    Engine.h
    Camera.cpp
    Camera.h
    DeviationColors.h
    Viewport.cpp
    Viewport.h
    ParamForm.cpp
    ParamForm.h
    MainWindow.cpp
    MainWindow.h)
target_include_directories(m2c_app PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(m2c_app
    PUBLIC
        Mesh2Cad::rpc Mesh2Cad::commands Mesh2Cad::document Mesh2Cad::core
        Qt6::Widgets Qt6::OpenGL Qt6::OpenGLWidgets Qt6::Network
    PRIVATE
        m2c_warnings)
# SpaceMouse navigation (ADR 0039), only with MESH2CAD_SPACEMOUSE=ON and the 3DxWare SDK.
if(M2C_HAVE_SPACEMOUSE)
    target_sources(m2c_app PRIVATE SpaceMouse.cpp SpaceMouse.h)
    # PUBLIC: MainWindow.h has a member only in this build.
    target_compile_definitions(m2c_app PUBLIC M2C_HAVE_SPACEMOUSE PRIVATE NOMINMAX)
    # The SDK's 3DxWare::Navlib target has no IMPORTED_LOCATION, so link its parts, as the SDK's
    # own samples do.
    target_link_libraries(m2c_app PRIVATE 3DxWare::headers ${TDxWare_LIBRARIES})
    # The SDK's library comes without its debug database.
    target_link_options(m2c_app INTERFACE /ignore:4099)
endif()
# The source tree's guide is Help > User guide's fallback in a build that isn't installed.
target_compile_definitions(m2c_app
    PRIVATE MESH2CAD_VERSION="${PROJECT_VERSION}" MESH2CAD_SOURCE_DIR="${PROJECT_SOURCE_DIR}")

add_executable(mesh2cad-app WIN32 main.cpp)
target_link_libraries(mesh2cad-app PRIVATE m2c_app m2c_warnings)
target_compile_definitions(mesh2cad-app PRIVATE MESH2CAD_VERSION="${PROJECT_VERSION}")
m2c_copy_runtime(mesh2cad-app)

m2c_deploy_qt(mesh2cad-app) # so it runs outside the IDE
