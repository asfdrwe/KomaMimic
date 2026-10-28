# SPDX-License-Identifier: GPL-3.0-only
# Homebrew ships the stable GLib API, but disables the Qt API.
# vcpkg provides GLib via poppler[glib]; it has no GLib CMake export target.
if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_HOST_TRIPLET)
    find_program(PKG_CONFIG_EXECUTABLE NAMES pkgconf pkg-config
        HINTS "${VCPKG_INSTALLED_DIR}/${VCPKG_HOST_TRIPLET}/tools/pkgconf")
endif()
find_package(PkgConfig REQUIRED)
add_library(komamimic_pdf_dependencies INTERFACE)
if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
    set(pdf_prefix "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
    set(pdf_old_path "$ENV{PKG_CONFIG_PATH}")
    set(pdf_old_libdir "$ENV{PKG_CONFIG_LIBDIR}")
    # Restrict queries to the target triplet; never mix MSYS2/system libraries.
    set(ENV{PKG_CONFIG_PATH} "")
    set(ENV{PKG_CONFIG_LIBDIR} "${pdf_prefix}/lib/pkgconfig")
    pkg_check_modules(PDF_RELEASE REQUIRED IMPORTED_TARGET
        NO_CMAKE_PATH NO_CMAKE_ENVIRONMENT_PATH poppler-glib>=22.0 cairo)
    if(EXISTS "${pdf_prefix}/debug/lib/pkgconfig/poppler-glib.pc")
        set(ENV{PKG_CONFIG_LIBDIR} "${pdf_prefix}/debug/lib/pkgconfig")
        pkg_check_modules(PDF_DEBUG REQUIRED IMPORTED_TARGET
            NO_CMAKE_PATH NO_CMAKE_ENVIRONMENT_PATH poppler-glib>=22.0 cairo)
        target_link_libraries(komamimic_pdf_dependencies INTERFACE
            "$<$<CONFIG:Debug>:PkgConfig::PDF_DEBUG>"
            "$<$<NOT:$<CONFIG:Debug>>:PkgConfig::PDF_RELEASE>")
    else()
        # A custom release-only vcpkg triplet has no debug package.
        target_link_libraries(komamimic_pdf_dependencies INTERFACE PkgConfig::PDF_RELEASE)
    endif()
    set(ENV{PKG_CONFIG_PATH} "${pdf_old_path}")
    set(ENV{PKG_CONFIG_LIBDIR} "${pdf_old_libdir}")
    unset(pdf_prefix)
    unset(pdf_old_path)
    unset(pdf_old_libdir)
else()
    pkg_check_modules(POPPLER REQUIRED IMPORTED_TARGET poppler-glib>=22.0 cairo)
    target_link_libraries(komamimic_pdf_dependencies INTERFACE PkgConfig::POPPLER)
endif()
