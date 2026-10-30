# SPDX-License-Identifier: GPL-3.0-only
add_library(test_support STATIC test_support.cpp)
target_link_libraries(test_support PUBLIC reader_ui Qt6::Test)
target_include_directories(test_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

set(KOMAMIMIC_TEST_SCOPE full CACHE STRING "full or platform integration checks")
set_property(CACHE KOMAMIMIC_TEST_SCOPE PROPERTY STRINGS full platform)
if(KOMAMIMIC_TEST_SCOPE STREQUAL "full")
    set(suites reader ui feature format pdf cli script pic document power evolution cache i18n archive_text legacy_settings)
elseif(KOMAMIMIC_TEST_SCOPE STREQUAL "platform")
    set(suites cli ui pdf script document power cache i18n)
    set(cache_functions zipCancellationDuringForwardSkip diskCacheSurvivesKilledWriter)
    set(ui_functions folderTreeListsFilesystemRoots closeCancelsCollectionCover fullscreenDefaultDoesNotOverlapAccept slideshowScrollDefault mangaDefaultsAndSavedSettings mangaMouseDefaultsAndScroll scriptPreferencesSaveCancelAndRestart loadNavigateAndViews toolbarDefaultsAndPersistence fullscreenChromeAtSeparateEdges fullscreenMarginsWithoutUpperBar shortcutScopesAndPersistence thumbnailShortcutScopesFollowFocus nativeApplicationMenuRoles directoryChangesRefreshCovers directoryRefreshPreservesEditingAndStopsAfterSettingsWrites)
    set(script_functions registryPreviewAndApiOneValidation scriptContractAndIsolation foldersAndArchives)
    set(document_functions epubUnicodeFilesystemPath defaultStandardDocuments fixedViewportRendering extendedWideImagesAndSharedSpace documentReflowAndResume)
    set(power_functions windowLifecycleAndCommonPreference generalPreferencesSurviveProcessRestart existingEscapedGeneralSectionIsReadAfterRestart)
else()
    message(FATAL_ERROR "Unknown KOMAMIMIC_TEST_SCOPE: ${KOMAMIMIC_TEST_SCOPE}")
endif()
foreach(suite IN LISTS suites)
    add_executable(${suite}_tests ${suite}_tests.cpp)
    target_link_libraries(${suite}_tests PRIVATE test_support)
    set_target_properties(${suite}_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    add_dependencies(${suite}_tests komamimic-script-worker)
    add_test(NAME ${suite}_tests COMMAND ${suite}_tests ${${suite}_functions})
    set_tests_properties(${suite}_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        TIMEOUT 90)
endforeach()
add_dependencies(ui_tests komamimic)
target_compile_definitions(ui_tests PRIVATE KOMAMIMIC_EXECUTABLE="$<TARGET_FILE:komamimic>")
add_dependencies(cli_tests komamimic)
add_dependencies(i18n_tests komamimic)
target_compile_definitions(i18n_tests PRIVATE KOMAMIMIC_EXECUTABLE="$<TARGET_FILE:komamimic>")
target_link_libraries(script_tests PRIVATE Threads::Threads)
if(TARGET cache_tests)
    target_link_libraries(cache_tests PRIVATE Threads::Threads)
endif()
target_link_libraries(document_tests PRIVATE Qt6::Qml)
target_compile_definitions(cli_tests PRIVATE
    KOMAMIMIC_EXECUTABLE="$<TARGET_FILE:komamimic>")

if(TARGET format_tests)
    add_test(NAME format_tests_c_locale COMMAND format_tests)
    set_tests_properties(format_tests_c_locale PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;LC_ALL=C;LANG=C"
        TIMEOUT 90)
endif()

if(TARGET Qt6::DBus AND UNIX AND NOT APPLE)
    target_link_libraries(power_tests PRIVATE Qt6::DBus)
    target_compile_definitions(power_tests PRIVATE KOMAMIMIC_TEST_DBUS=1)
endif()

if(TARGET evolution_tests)
    target_link_libraries(evolution_tests PRIVATE Qt6::Qml)
endif()

if(KOMAMIMIC_TEST_SCOPE STREQUAL "full")
    add_executable(archive_benchmark archive_benchmark.cpp)
    target_link_libraries(archive_benchmark PRIVATE reader_core)
    set_target_properties(archive_benchmark PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
endif()

add_executable(validation_probe validation_probe.cpp)
target_link_libraries(validation_probe PRIVATE reader_core)
set_target_properties(validation_probe PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_dependencies(validation_probe komamimic-script-worker)

add_executable(document_probe document_probe.cpp)
target_link_libraries(document_probe PRIVATE reader_ui)
set_target_properties(document_probe PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
add_test(NAME distribution_tests COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/test_distribution.py"
    --worker "$<TARGET_FILE:komamimic-script-worker>" --probe "$<TARGET_FILE:validation_probe>")
set_tests_properties(distribution_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    TIMEOUT 120)
