cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake")

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" ON)
option(ENABLE_QT "Use Qt functionality" OFF)

include(compilerconfig)
include(defaults)
include(helpers)

# Create the OBS::libobs imported target. On Linux this is done inside the
# template's defaults.cmake, but the macOS defaults do not call find_package,
# so we do it explicitly here. On Windows the template handles this too, and
# find_package(libobs) is harmless when the target already exists; it returns
# from cache.
find_package(libobs REQUIRED)

# Frontend API: we always use it (preview/program detection, dock). Find it
# unconditionally (the build preset forces ENABLE_FRONTEND_API off otherwise).
find_package(obs-frontend-api REQUIRED)

# ── Qt6 ────────────────────────────────────────────────────────────────────
# Qt6 is needed for the on-screen control dock.
#
# macOS: the obs-deps Qt6 universal bundle lives nested under .deps/, so we
# point CMAKE_PREFIX_PATH at it. We also have to work around a quirk of macOS
# 26 / Xcode 26 where the legacy AGL framework is gone but Qt's bundled
# FindWrapOpenGL.cmake still tries to link "-framework AGL". The pre-seed +
# scrub below avoid that link error. None of this applies on Windows.
#
# Windows: the OBS plugin template's Windows build pulls Windows obs-deps
# (including Qt6) and adds them to CMAKE_PREFIX_PATH automatically. We just
# need to find_package — no path overrides, no AGL workaround.
if(APPLE)
  find_library(WrapOpenGL_AGL NAMES OpenGL)
  list(APPEND CMAKE_PREFIX_PATH
    "${CMAKE_CURRENT_SOURCE_DIR}/.deps/obs-deps-qt6-2025-07-11-universal")
endif()

find_package(Qt6 REQUIRED COMPONENTS Widgets)

if(APPLE)
  # Backup scrub: if AGL still slipped into any target's link interface
  # (despite the pre-seed above), strip it before link.
  foreach(_agl_tgt OpenGL::GL Qt6::Gui WrapOpenGL::WrapOpenGL)
    if(TARGET ${_agl_tgt})
      get_target_property(_agl_libs ${_agl_tgt} INTERFACE_LINK_LIBRARIES)
      if(_agl_libs)
        list(REMOVE_ITEM _agl_libs "-framework AGL" "AGL" "-framework  AGL")
        set_target_properties(${_agl_tgt} PROPERTIES
          INTERFACE_LINK_LIBRARIES "${_agl_libs}")
      endif()
    endif()
  endforeach()
endif()

add_library(${CMAKE_PROJECT_NAME} MODULE)

# ── Plugin sources ─────────────────────────────────────────────────────────
target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/plugin-main.c
    src/odp-source.c
    src/odp-export.c
    src/odp-export.h
    src/odp-dock.cpp
)

target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)

# On macOS the frontend-api imported target doesn't always propagate its
# include directory, so add it explicitly. On Windows the template handles
# this correctly so the extra path is unnecessary (and the directory may not
# even exist), so guard it.
if(APPLE)
  target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/.deps/include/obs")
endif()

# Qt needs AUTOMOC to run on the C++ dock source. Link Widgets for the dock's
# buttons, layouts, labels, frames.
set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES AUTOMOC ON)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Widgets)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
