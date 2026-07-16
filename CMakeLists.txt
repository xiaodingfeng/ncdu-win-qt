cmake_minimum_required(VERSION 3.16)

project(NcduWin VERSION 1.0.1 LANGUAGES CXX)

# Set NcduWin as the default startup target in Visual Studio
set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT NcduWin)

# ---- Single source of truth for app metadata ----
set(APP_NAME        "NcduWin")
set(APP_VERSION     "1.0.1")
set(APP_HOMEPAGE    "https://ncduwin.fengzhengx.cn/")
set(APP_SOURCE_URL  "https://github.com/xiaodingfeng/ncdu-win-qt")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# ---- Auto-detect Qt6 if CMAKE_PREFIX_PATH is not already set ----
if(NOT CMAKE_PREFIX_PATH AND NOT Qt6_DIR)
    file(GLOB QT_VERSIONS LIST_DIRECTORIES true "C:/Qt/6.*")
    if(QT_VERSIONS)
        list(SORT QT_VERSIONS)
        list(REVERSE QT_VERSIONS)
        foreach(QT_VER ${QT_VERSIONS})
            if(IS_DIRECTORY "${QT_VER}/msvc2022_64")
                set(CMAKE_PREFIX_PATH "${QT_VER}/msvc2022_64")
                message(STATUS "Auto-detected Qt6: ${CMAKE_PREFIX_PATH}")
                break()
            elseif(IS_DIRECTORY "${QT_VER}/msvc2019_64")
                set(CMAKE_PREFIX_PATH "${QT_VER}/msvc2019_64")
                message(STATUS "Auto-detected Qt6: ${CMAKE_PREFIX_PATH}")
                break()
            endif()
        endforeach()
    endif()
endif()

find_package(Qt6 COMPONENTS Widgets Concurrent REQUIRED)
find_package(Qt6 COMPONENTS Test QUIET)

# ---- Locate windeployqt for the current Qt installation ----
get_target_property(_qt6_qmake Qt6::qmake IMPORTED_LOCATION)
get_filename_component(QT_BIN_DIR "${_qt6_qmake}" DIRECTORY)

# ---- Generate version header for C++ ----
configure_file(src/version.h.in ${CMAKE_BINARY_DIR}/generated/version.h @ONLY)

# ---- Generate version include for Inno Setup installer ----
configure_file(scripts/version.iss.in ${CMAKE_SOURCE_DIR}/scripts/version.iss @ONLY)

# Clean up any .vs/ that VS may have dropped in the source tree
file(REMOVE_RECURSE "${CMAKE_SOURCE_DIR}/src/.vs")

set(SOURCES
    src/main.cpp
    src/core/DiskScanner.cpp
    src/core/FormatHelpers.cpp
    src/core/I18n.cpp
    src/core/Identify.cpp
    src/core/WinApi.cpp
    src/ui/MainWindow.cpp
    src/ui/SizeBarDelegate.cpp
    src/ui/TreemapWidget.cpp
    src/ui/BreadcrumbBar.cpp
    src/ui/LegendBar.cpp
    src/cleanup/CleanupScanner.cpp
    src/cleanup/CleanupWorker.cpp
    src/cleanup/CleanupPanel.cpp
)

file(COPY locales DESTINATION ${CMAKE_BINARY_DIR})

if(WIN32)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/app.ico")
        set(APP_RC "${CMAKE_BINARY_DIR}/app.rc")
        file(WRITE ${APP_RC} "IDI_ICON1 ICON \"${CMAKE_CURRENT_SOURCE_DIR}/app.ico\"\n")
        list(APPEND SOURCES ${APP_RC})
    endif()
endif()

add_executable(NcduWin WIN32 ${SOURCES})

set_target_properties(NcduWin PROPERTIES OUTPUT_NAME "NcduWin")

target_include_directories(NcduWin PRIVATE
    ${CMAKE_BINARY_DIR}/generated
    src
    src/core
    src/ui
    src/cleanup
)

target_link_libraries(NcduWin PRIVATE Qt6::Widgets Qt6::Concurrent)

if(WIN32)
    target_link_libraries(NcduWin PRIVATE shell32 shlwapi ole32 user32 kernel32 advapi32)
    if(MSVC)
        set_target_properties(NcduWin PROPERTIES
            LINK_FLAGS "/MANIFEST:EMBED /MANIFESTINPUT:\"${CMAKE_CURRENT_SOURCE_DIR}/app.manifest\" /MANIFESTUAC:NO"
        )
    endif()
endif()

install(TARGETS NcduWin RUNTIME DESTINATION dist)

add_custom_command(TARGET NcduWin POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/locales"
            "$<TARGET_FILE_DIR:NcduWin>/locales"
    VERBATIM
)

# Deploy Qt DLLs matching the build configuration (Debug→debug DLLs, Release→release DLLs)
add_custom_command(TARGET NcduWin POST_BUILD
    COMMAND "${QT_BIN_DIR}/windeployqt.exe"
            "$<TARGET_FILE:NcduWin>"
            $<IF:$<CONFIG:Debug>,--debug,--release>
            --no-translations --no-system-d3d-compiler --no-opengl-sw
    COMMENT "Deploying Qt dependencies ($<CONFIG>)..."
    # Remove large DX shader DLLs not needed by this application
    COMMAND ${CMAKE_COMMAND} -E remove
            "$<TARGET_FILE_DIR:NcduWin>/dxcompiler.dll"
            "$<TARGET_FILE_DIR:NcduWin>/dxil.dll"
)

if(Qt6Test_FOUND)
    enable_testing()
    add_executable(test_scanner
        tests/test_scanner.cpp
        src/core/FormatHelpers.cpp
    )
    target_include_directories(test_scanner PRIVATE src src/core src/ui src/cleanup)
    target_link_libraries(test_scanner PRIVATE Qt6::Test Qt6::Concurrent)
    if(WIN32)
        target_link_libraries(test_scanner PRIVATE kernel32 advapi32)
    endif()
    add_test(NAME test_scanner COMMAND test_scanner)
endif()
