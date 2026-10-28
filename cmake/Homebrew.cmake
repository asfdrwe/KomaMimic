# SPDX-License-Identifier: GPL-3.0-only
# Keg-only formulae are not exposed by Homebrew's bin directory in PATH.
# Keep explicit package roots and CMAKE_PREFIX_PATH ahead of these defaults.
if(NOT APPLE OR CMAKE_CROSSCOMPILING)
    return()
endif()

option(KOMAMIMIC_USE_HOMEBREW "Search installed Homebrew dependency prefixes" ON)
if(NOT KOMAMIMIC_USE_HOMEBREW)
    return()
endif()

find_program(KOMAMIMIC_BREW_EXECUTABLE brew)
if(NOT KOMAMIMIC_BREW_EXECUTABLE)
    return()
endif()

set(komamimic_brew_formulas qtbase qtdeclarative qttools qttranslations qtwebengine qtwebchannel qtpositioning qtshadertools qtsvg qtlanguageserver libarchive icu4c sdl2-compat poppler glib cairo)
foreach(formula IN LISTS komamimic_brew_formulas)
    execute_process(
        COMMAND "${KOMAMIMIC_BREW_EXECUTABLE}" --prefix "${formula}"
        RESULT_VARIABLE brew_result
        OUTPUT_VARIABLE brew_prefix
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        TIMEOUT 10)
    # --prefix can report an uninstalled formula's prospective path.
    if(brew_result STREQUAL "0" AND IS_DIRECTORY "${brew_prefix}/include")
        list(APPEND CMAKE_PREFIX_PATH "${brew_prefix}")
        # Qt restricts component searches to its own installation and this list.
        # Homebrew installs Qml, WebEngine and their Qt dependencies separately.
        if(formula MATCHES "^qt")
            list(APPEND QT_ADDITIONAL_PACKAGES_PREFIX_PATH "${brew_prefix}")
        endif()
        message(STATUS "Homebrew ${formula}: ${brew_prefix}")
    endif()
endforeach()
list(REMOVE_DUPLICATES CMAKE_PREFIX_PATH)
list(REMOVE_DUPLICATES QT_ADDITIONAL_PACKAGES_PREFIX_PATH)
unset(brew_result)
unset(brew_prefix)
unset(formula)
