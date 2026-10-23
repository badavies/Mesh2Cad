# Install rules and installers (issue #122): an NSIS installer and a ZIP on Windows, a .deb and a
# .tar.gz on Linux, made with `cpack` from a Release build. .github/workflows/package.yml builds
# them and installs each one on a clean machine.

include(GNUInstallDirs)

set(m2c_programs mesh2cad mesh2cad-rpc mesh2cad-mcp)
if(TARGET mesh2cad-app)
    list(APPEND m2c_programs mesh2cad-app)
endif()

if(WIN32)
    # Everything in bin, as Windows looks for DLLs next to the executable. The DLLs are found the
    # way Windows finds them; Qt's are left to windeployqt, which also copies its plugins.
    install(TARGETS ${m2c_programs} RUNTIME DESTINATION bin)
    set(m2c_exes)
    foreach(program IN LISTS m2c_programs)
        list(APPEND m2c_exes "$<TARGET_FILE:${program}>")
    endforeach()
    set(m2c_dll_dirs)
    if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
        list(APPEND m2c_dll_dirs "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin")
    endif()
    # Not install(RUNTIME_DEPENDENCY_SET): each program's folder has its own copy of the DLLs
    # (m2c_copy_runtime, vcpkg), which it reports as conflicting paths. The copies are the same
    # files, so the first is installed.
    install(CODE "
        file(GET_RUNTIME_DEPENDENCIES
            EXECUTABLES ${m2c_exes}
            DIRECTORIES ${m2c_dll_dirs}
            PRE_EXCLUDE_REGEXES \"^api-ms-\" \"^ext-ms-\" \"^[Qq]t6\"
            POST_EXCLUDE_REGEXES \"[/\\\\\\\\][Ss]ystem32[/\\\\\\\\]\" \"[/\\\\\\\\][Ww]indows[/\\\\\\\\]\"
            RESOLVED_DEPENDENCIES_VAR m2c_dlls
            UNRESOLVED_DEPENDENCIES_VAR m2c_missing
            CONFLICTING_DEPENDENCIES_PREFIX m2c_conflict)
        if(m2c_missing)
            message(FATAL_ERROR \"DLLs not found: \${m2c_missing}\")
        endif()
        foreach(name IN LISTS m2c_conflict_FILENAMES)
            list(GET m2c_conflict_\${name} 0 first)
            list(APPEND m2c_dlls \"\${first}\")
        endforeach()
        file(INSTALL \${m2c_dlls} DESTINATION \"\${CMAKE_INSTALL_PREFIX}/bin\")")
    if(TARGET mesh2cad-app AND M2C_WINDEPLOYQT)
        install(CODE "
            execute_process(
                COMMAND \"${M2C_WINDEPLOYQT}\" --release --no-translations --no-compiler-runtime
                        \"\${CMAKE_INSTALL_PREFIX}/bin/mesh2cad-app.exe\"
                COMMAND_ERROR_IS_FATAL ANY)")
    endif()
    # The Visual C++ runtime, for machines without the redistributable.
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION bin)
    include(InstallRequiredSystemLibraries)
    set(m2c_doc_dir doc)
else()
    install(TARGETS ${m2c_programs} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    if(TARGET mesh2cad-app)
        install(FILES "${CMAKE_SOURCE_DIR}/packaging/linux/mesh2cad.desktop"
                DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    endif()
    # Not share/doc, which minimal installs (Docker images, WSL) leave out; the examples are
    # needed to follow the user guide.
    set(m2c_doc_dir ${CMAKE_INSTALL_DATADIR}/mesh2cad)
endif()

install(FILES LICENSE README.md THIRD_PARTY.md docs/COMMANDS.md DESTINATION ${m2c_doc_dir})
install(DIRECTORY examples/ DESTINATION ${m2c_doc_dir}/examples)

# ---- CPack ---------------------------------------------------------------------------------------
set(CPACK_PACKAGE_NAME "mesh2cad")
set(CPACK_PACKAGE_VENDOR "Mesh2Cad")
set(CPACK_PACKAGE_CONTACT "Mesh2Cad <https://github.com/badavies/Mesh2Cad>")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/badavies/Mesh2Cad")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "Mesh2Cad")
set(CPACK_PACKAGE_CHECKSUM SHA256)
set(CPACK_STRIP_FILES ON)
# NSIS shows the licence before installing and wants a .txt.
configure_file(LICENSE "${CMAKE_BINARY_DIR}/LICENSE.txt" COPYONLY)
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_BINARY_DIR}/LICENSE.txt")

if(WIN32)
    set(CPACK_GENERATOR NSIS ZIP)
    set(CPACK_PACKAGE_FILE_NAME "mesh2cad-${PROJECT_VERSION}-windows-x64")
    set(CPACK_NSIS_PACKAGE_NAME "Mesh2Cad ${PROJECT_VERSION}")
    set(CPACK_NSIS_DISPLAY_NAME "Mesh2Cad")
    set(CPACK_NSIS_MODIFY_PATH ON) # offers to put bin on the PATH, for the CLI and MCP server
    set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
    set(CPACK_NSIS_URL_INFO_ABOUT "${CPACK_PACKAGE_HOMEPAGE_URL}")
    if(TARGET mesh2cad-app)
        set(CPACK_PACKAGE_EXECUTABLES "mesh2cad-app;Mesh2Cad")
        set(CPACK_NSIS_INSTALLED_ICON_NAME "bin\\\\mesh2cad-app.exe")
    endif()
else()
    set(CPACK_GENERATOR DEB TGZ)
    set(CPACK_PACKAGE_FILE_NAME "mesh2cad-${PROJECT_VERSION}-linux-x64")
    set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
    set(CPACK_DEBIAN_PACKAGE_SECTION science)
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON) # the system libraries it links, found by dpkg-shlibdeps
    # The summary is the first line; this follows it.
    string(CONCAT CPACK_DEBIAN_PACKAGE_DESCRIPTION
        "Imports STL, OBJ and PLY scans, repairs and aligns them, fits B-spline surfaces and\n"
        "exact primitives, and exports STEP for CAD and GLB, OBJ and STL meshes.")
endif()

include(CPack)
