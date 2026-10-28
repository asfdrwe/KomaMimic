# SPDX-License-Identifier: GPL-3.0-only
# Embed PNGs to avoid adding a Qt SVG dependency at runtime.
target_sources(komamimic PRIVATE src/applicationicons.qrc)
if(WIN32)
    enable_language(RC)
    configure_file(cmake/applicationicon.rc.in applicationicon.rc @ONLY)
    target_sources(komamimic PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/applicationicon.rc")
elseif(APPLE)
    # Ready for the later .app packaging step; do not change current executable paths.
    set_source_files_properties(assets/icons/komamimic.icns PROPERTIES
        MACOSX_PACKAGE_LOCATION Resources)
    target_sources(komamimic PRIVATE assets/icons/komamimic.icns)
    set_target_properties(komamimic PROPERTIES MACOSX_BUNDLE_ICON_FILE komamimic.icns)
else()
    foreach(size IN ITEMS 16 24 32 48 64 128 256 512 1024)
        install(FILES "assets/icons/komamimic-${size}.png"
            DESTINATION "share/icons/hicolor/${size}x${size}/apps" RENAME komamimic.png)
    endforeach()
    install(FILES assets/icons/komamimic-symbol.svg
        DESTINATION share/icons/hicolor/scalable/apps RENAME komamimic.svg)
    install(FILES assets/komamimic.desktop DESTINATION share/applications)
endif()
