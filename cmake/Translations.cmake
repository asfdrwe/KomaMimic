# SPDX-License-Identifier: GPL-3.0-only
# Add a komamimic_<locale>.ts catalog; registration and embedding are automatic.
find_package(Qt6 6.5 REQUIRED COMPONENTS LinguistTools)
file(GLOB KOMAMIMIC_TRANSLATION_SOURCES CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/translations/komamimic_*.ts")
set(KOMAMIMIC_LANGUAGES "[\"ja\"")
foreach(catalog IN LISTS KOMAMIMIC_TRANSLATION_SOURCES)
    get_filename_component(stem "${catalog}" NAME_WE)
    string(REGEX REPLACE "^komamimic_" "" code "${stem}")
    if(NOT code MATCHES "^[a-z][a-z][a-z]?([_-][A-Za-z0-9]+)*$" OR code STREQUAL "ja")
        message(FATAL_ERROR "Invalid or duplicate source-language catalog: ${catalog}")
    endif()
    string(APPEND KOMAMIMIC_LANGUAGES ",\"${code}\"")
endforeach()
string(APPEND KOMAMIMIC_LANGUAGES "]\n")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/languages.json" "${KOMAMIMIC_LANGUAGES}")
qt_add_lrelease(TS_FILES ${KOMAMIMIC_TRANSLATION_SOURCES} QM_FILES_OUTPUT_VARIABLE KOMAMIMIC_QM_FILES)
qt_add_resources(reader_core i18n_catalogs PREFIX "/i18n"
    BASE "${CMAKE_CURRENT_BINARY_DIR}"
    FILES ${KOMAMIMIC_QM_FILES} "${CMAKE_CURRENT_BINARY_DIR}/languages.json")
file(GLOB KOMAMIMIC_TRANSLATABLE_SOURCES CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/src/*.cpp" "${PROJECT_SOURCE_DIR}/src/*.h")
qt_add_lupdate(reader_core TS_FILES ${KOMAMIMIC_TRANSLATION_SOURCES}
    SOURCES ${KOMAMIMIC_TRANSLATABLE_SOURCES} OPTIONS -no-obsolete)
