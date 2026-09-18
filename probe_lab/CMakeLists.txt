# Standalone probe lab for NcduWin (kept OUT of the app build).
# Lives in the project tree so Windows temp-cleanup cannot wipe it again
# (the old %TEMP%/ncdu_panel_test workspace was lost twice in one day).
# Build:
#   cmake -S probe_lab -B probe_lab/build -G "Visual Studio 17 2022" -A x64 \
#         -DCMAKE_PREFIX_PATH=C:/Qt/6.11.1/msvc2022_64
#   cmake --build probe_lab/build --config Release
# Run each probe from its output dir (logs are written to the process CWD).
#
# probe_lab/build/ is disposable output (also git-ignored); everything else here
# is the regression range and is worth keeping.
#
# resources/dont_delete_folder.ico is a DELIBERATE copy of the app's icon: a
# qt_add_resources() FILES entry must be a path relative to the target's source
# dir, and ${PROJ}/resources/... is not, so the probes link their own copy of
# the very same bytes. Do not "de-duplicate" it away.
cmake_minimum_required(VERSION 3.21)
project(ncdu_probe_lab LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(PROJ D:/dingfeng/work/aiproject/ncdu-win)

find_package(Qt6 REQUIRED COMPONENTS Core)

add_executable(dstdup_probe run2/dstdup_probe.cpp)
target_include_directories(dstdup_probe PRIVATE ${PROJ}/src/core)
target_link_libraries(dstdup_probe PRIVATE Qt6::Core)

# Both of these compile the REAL WinApi.cpp + Logger.cpp against the real
# headers, so a probe failure is a product failure, not a fixture artifact.
# I18n.cpp comes along because the marker writer takes its file name from the
# active language (locales/*.json) instead of a hard-coded string.
add_executable(movefile_probe run1/movefile_probe.cpp
    ${PROJ}/src/core/WinApi.cpp
    ${PROJ}/src/core/Logger.cpp
    ${PROJ}/src/core/I18n.cpp)
target_include_directories(movefile_probe PRIVATE ${PROJ}/src ${PROJ}/src/core)
target_link_libraries(movefile_probe PRIVATE Qt6::Core
    shell32 shlwapi ole32 user32 kernel32 advapi32 rstrtmgr)

add_executable(lockproc_probe run3/lockproc_probe.cpp
    ${PROJ}/src/core/WinApi.cpp
    ${PROJ}/src/core/Logger.cpp
    ${PROJ}/src/core/I18n.cpp)
target_include_directories(lockproc_probe PRIVATE ${PROJ}/src ${PROJ}/src/core)
target_link_libraries(lockproc_probe PRIVATE Qt6::Core
    shell32 shlwapi ole32 user32 kernel32 advapi32 rstrtmgr)

# Links the same qrc resource the app embeds, so the marker extraction path
# (resource -> user-visible file) is tested for real, together with the
# per-language file name. Needs locales/ next to the exe (run from
# build/Release after `cp -r ../../../locales .`).
add_executable(marker_probe run4/marker_probe.cpp
    ${PROJ}/src/core/WinApi.cpp
    ${PROJ}/src/core/Logger.cpp
    ${PROJ}/src/core/I18n.cpp)
qt_add_resources(marker_probe "moveguard"
    PREFIX "/moveguard"
    FILES resources/dont_delete_folder.ico)
target_include_directories(marker_probe PRIVATE ${PROJ}/src ${PROJ}/src/core)
target_link_libraries(marker_probe PRIVATE Qt6::Core
    shell32 shlwapi ole32 user32 kernel32 advapi32 rstrtmgr)

# Header-only: the library-folder table, its path resolution and the default
# scan roots are exactly the code the app compiles, so what this asserts is the
# product's behaviour. Needs locales/ next to the exe (copied in before running).
add_executable(knownfolder_probe run5/knownfolder_probe.cpp)
target_include_directories(knownfolder_probe PRIVATE ${PROJ}/src ${PROJ}/src/core)
target_link_libraries(knownfolder_probe PRIVATE Qt6::Core
    shell32 ole32 user32 kernel32)

# Renders the move panel headlessly (no show(), so nothing scans the profile)
# and writes a PNG for eyeball review of the warning block. Compiles the REAL
# AppDataMovePanel.cpp + the same qrc the app embeds.
find_package(Qt6 REQUIRED COMPONENTS Widgets Concurrent)
add_executable(ui_render_probe run6/ui_render_probe.cpp
    ${PROJ}/src/ui/AppDataMovePanel.cpp
    ${PROJ}/src/ui/AppPathSyncDialog.cpp
    ${PROJ}/src/core/WinApi.cpp
    ${PROJ}/src/core/Logger.cpp
    ${PROJ}/src/core/I18n.cpp
    ${PROJ}/src/core/FormatHelpers.cpp)
qt_add_resources(ui_render_probe "moveguard"
    PREFIX "/moveguard"
    FILES resources/dont_delete_folder.ico)
# The panel is a QObject: its moc output has to be generated for this target too.
set_target_properties(ui_render_probe PROPERTIES AUTOMOC ON)
target_include_directories(ui_render_probe PRIVATE ${PROJ}/src ${PROJ}/src/core ${PROJ}/src/ui)
target_link_libraries(ui_render_probe PRIVATE Qt6::Widgets Qt6::Concurrent
    shell32 shlwapi ole32 user32 kernel32 advapi32 rstrtmgr)

# Language refresh: builds the real relocation dialog, software-data panel and AI
# window, switches language on an already-open set of windows and walks every
# readable string looking for leftovers. Also audits MainWindow's cached widgets
# against its retranslateUI() — for that it needs to read the sources, hence
# NCDU_SRC_DIR. Needs locales/ next to the exe.
add_executable(lang_refresh_probe run7/lang_refresh_probe.cpp
    ${PROJ}/src/ui/AppDataMovePanel.cpp
    ${PROJ}/src/ui/AppPathSyncDialog.cpp
    ${PROJ}/src/ai/AiAnalysisDialog.cpp
    ${PROJ}/src/core/WinApi.cpp
    ${PROJ}/src/core/Logger.cpp
    ${PROJ}/src/core/I18n.cpp
    ${PROJ}/src/core/FormatHelpers.cpp)
qt_add_resources(lang_refresh_probe "moveguard"
    PREFIX "/moveguard"
    FILES resources/dont_delete_folder.ico)
set_target_properties(lang_refresh_probe PROPERTIES AUTOMOC ON)
target_compile_definitions(lang_refresh_probe PRIVATE NCDU_SRC_DIR="${PROJ}/src")
target_include_directories(lang_refresh_probe PRIVATE ${PROJ}/src ${PROJ}/src/core ${PROJ}/src/ui ${PROJ}/src/ai)
target_link_libraries(lang_refresh_probe PRIVATE Qt6::Widgets Qt6::Concurrent
    shell32 shlwapi ole32 user32 kernel32 advapi32 rstrtmgr)
