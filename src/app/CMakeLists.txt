# Desktop app (ADR 0023). Built only when Qt 6 is found; see README "Building the app".
set(CMAKE_AUTOMOC ON)

# Everything but main(), so tests/app can drive the real window.
add_library(m2c_app STATIC
    Engine.cpp
    Engine.h
    Camera.cpp
    Camera.h
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
target_compile_definitions(m2c_app PRIVATE MESH2CAD_VERSION="${PROJECT_VERSION}")

add_executable(mesh2cad-app WIN32 main.cpp)
target_link_libraries(mesh2cad-app PRIVATE m2c_app m2c_warnings)
target_compile_definitions(mesh2cad-app PRIVATE MESH2CAD_VERSION="${PROJECT_VERSION}")
m2c_copy_runtime(mesh2cad-app)

if(WIN32)
    # Copy the Qt DLLs next to the exe so it runs outside the IDE (windeployqt ships with Qt).
    find_program(M2C_WINDEPLOYQT windeployqt HINTS "${Qt6_DIR}/../../../bin")
    if(M2C_WINDEPLOYQT)
        add_custom_command(TARGET mesh2cad-app POST_BUILD
            COMMAND "${M2C_WINDEPLOYQT}" --no-translations --no-compiler-runtime
                    "$<TARGET_FILE:mesh2cad-app>"
            COMMENT "Copying Qt libraries next to mesh2cad-app")
    endif()
endif()
