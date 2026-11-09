# SPDX-License-Identifier: GPL-3.0-only
add_library(test_support STATIC test_support.cpp)
target_link_libraries(test_support PUBLIC reader_ui Qt6::Test)
target_include_directories(test_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

set(KOMAMIMIC_TEST_SCOPE full CACHE STRING "full or platform integration checks")
set_property(CACHE KOMAMIMIC_TEST_SCOPE PROPERTY STRINGS full platform)
if(KOMAMIMIC_TEST_SCOPE STREQUAL "full")
    set(suites compatibility reader ui feature format pdf cli script pic document power evolution cache i18n archive_text legacy_settings thumbnail thumbnail_scroll reader_startup document_behavior)
elseif(KOMAMIMIC_TEST_SCOPE STREQUAL "platform")
    # Windows CI must exercise the settings.ini replacement regression too.
    set(suites compatibility cli ui pdf script document power cache i18n legacy_settings thumbnail thumbnail_scroll reader_startup document_behavior)
    set(cache_functions failedReplacementPreservesCompletedPendingPixels lateOldGenerationCannotReplaceNewPixels freshGenerationReplacesRecoveredJournal obsoleteNativeWorkDoesNotOccupyCurrentWorker warmReadsPreserveCreationTime capacityUsesCreationOrder cacheDeliveryPrecedesQueuedSourceAndStoreWork zipCancellationDuringForwardSkip diskCacheSurvivesKilledWriter archiveMetadataScanCanYield cacheLookupHasReservedWorker obsoleteWaitingJobsAreRemoved backgroundDoesNotOccupyDisplayWorker deferredDisplayDoesNotWaitForInventory deferredStoresRespectClearAndMemoryBound cacheHitsDoNotDiscardPendingImages deferredPressurePreservesEveryImage failedStoreRetainsRetryAndOldPixels lateStoreCancellationIsRetried journalSurvivesRestartAndConfiguration visibleWorkPrecedesUpdateStoreAndBackground invisibleRequestsAreDiscarded cancelledRunningRequestNeverRestarts visibleValidationSurvivesNewCacheRequests persistenceDoesNotRecheckSource)
    set(ui_functions explicitRefreshBypassesSavedCover visibleCachedCoverBypassesSlowGeneration pageModeDoesNotRenderHiddenReader folderTreeListsFilesystemRoots closeCancelsCollectionCover fullscreenDefaultDoesNotOverlapAccept slideshowScrollDefault mangaDefaultsAndSavedSettings mangaMouseDefaultsAndScroll scriptPreferencesSaveCancelAndRestart scriptRegistrationRestartNotice loadNavigateAndViews toolbarDefaultsAndPersistence restoreAndActivateBringsWindowForward restoreAndActivateKeepsWindowAndReadingState fullscreenPersistenceAndNormalWindow fullscreenPreferencesApplyAndCancel fullscreenSurvivesProcessRestart fullscreenStartupRequestsKeepCurrentState fullscreenChromeAtSeparateEdges fullscreenMarginsWithoutUpperBar shortcutScopesAndPersistence thumbnailShortcutScopesFollowFocus nativeApplicationMenuRoles directoryChangesRefreshCovers directoryRefreshPreservesEditingAndStopsAfterSettingsWrites)
    set(script_functions registryPreviewAndApiOneValidation scriptContractAndIsolation foldersAndArchives)
    set(document_functions documentProbeSeparateReport epubUnicodeFilesystemPath defaultStandardDocuments fixedViewportRendering extendedWideImagesAndSharedSpace documentReflowAndResume)
    set(power_functions windowLifecycleAndCommonPreference generalPreferencesSurviveProcessRestart existingEscapedGeneralSectionIsReadAfterRestart)
else()
    message(FATAL_ERROR "Unknown KOMAMIMIC_TEST_SCOPE: ${KOMAMIMIC_TEST_SCOPE}")
endif()
foreach(suite IN LISTS suites)
    add_executable(${suite}_tests ${suite}_tests.cpp)
    target_link_libraries(${suite}_tests PRIVATE test_support)
    set_target_properties(${suite}_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    add_dependencies(${suite}_tests komamimic-script-worker)
    add_test(NAME ${suite}_tests COMMAND ${suite}_tests ${${suite}_functions}
        -o -,txt -o ${suite}_tests.qtest.txt,txt)
    set_tests_properties(${suite}_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_FORCE_STDERR_LOGGING=1"
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
        TIMEOUT 90)
endforeach()
# UI persistence cases include disk syncs and fresh-process startup on slow disks.
set_tests_properties(ui_tests PROPERTIES TIMEOUT 180)
# The disk-cache count regression populates more than 20,000 real files.
set_tests_properties(cache_tests PROPERTIES TIMEOUT 300)
# Give the separate 220-folder/ZIP scroll fixtures their own bounded budget.
set_tests_properties(thumbnail_tests thumbnail_scroll_tests reader_startup_tests document_behavior_tests PROPERTIES TIMEOUT 180)
# These suites deliberately fsync many PNGs, including shutdown journals. Running
# them together measures storage contention rather than their bounded UI waits.
# Keep the existing time budgets and all assertions.
set_tests_properties(ui_tests cache_tests thumbnail_tests thumbnail_scroll_tests reader_startup_tests document_behavior_tests PROPERTIES RESOURCE_LOCK thumbnail_persistence_io)
# UI also starts fresh processes and approaches its budget on slow storage even
# alongside smaller suites. Run it alone; remaining suites can run in parallel.
set_tests_properties(ui_tests PROPERTIES RUN_SERIAL TRUE)
# Parallel WebEngine and filesystem-heavy tests need more time on Windows.
if(WIN32)
    # A full UI run took 274 seconds even without another suite running.
    set_tests_properties(ui_tests PROPERTIES TIMEOUT 600)
    # CLI subprocesses include native GUI startup and settings/history writes.
    set_tests_properties(cli_tests PROPERTIES TIMEOUT 180)
    set_tests_properties(document_tests cache_tests PROPERTIES TIMEOUT 300)
endif()
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
    add_test(NAME format_tests_c_locale COMMAND format_tests
        -o -,txt -o format_tests_c_locale.qtest.txt,txt)
    set_tests_properties(format_tests_c_locale PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_FORCE_STDERR_LOGGING=1;LC_ALL=C;LANG=C"
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
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
add_dependencies(document_tests document_probe)

find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)

# Smoke-test real secondary-process IPC with a private configuration.
# The offscreen runner cannot validate the compositor's focus-stealing policy.
add_test(NAME single_instance_tests
    COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/test_single_instance.py"
    --executable "$<TARGET_FILE:komamimic>")
set_tests_properties(single_instance_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_FORCE_STDERR_LOGGING=1"
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    TIMEOUT 120)

add_test(NAME distribution_tests COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/test_distribution.py"
    --worker "$<TARGET_FILE:komamimic-script-worker>" --probe "$<TARGET_FILE:validation_probe>")
set_tests_properties(distribution_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    TIMEOUT 120)

# Optional engine-independent execution of the actual asynchronous controller.
# Native WebEngine cases remain part of document_behavior_tests on every build.
find_program(KOMAMIMIC_NODE_EXECUTABLE NAMES node nodejs)
if(KOMAMIMIC_NODE_EXECUTABLE)
    add_test(NAME document_layout_tests COMMAND "${KOMAMIMIC_NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/document_layout_tests.js")
    set_tests_properties(document_layout_tests PROPERTIES TIMEOUT 30)
endif()
