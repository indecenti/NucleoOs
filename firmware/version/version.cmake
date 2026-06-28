# NucleoOS firmware version — the SINGLE SOURCE OF TRUTH.
#
# This composes PROJECT_VER, which ESP-IDF bakes into the application descriptor
# (esp_app_desc_t.version). Every surface that reports a version reads it back from there
# via esp_app_get_description() — /api/status, /proc/version, the mDNS TXT record, the
# serial boot banner, ANIMA's "che versione?" answer — so they can never disagree.
#
# Composition:   <semver>+<build>.g<git-short>[*]
#   <semver>     firmware/version/VERSION   — human-set, bumped on a real release (major/minor/patch)
#   <build>      firmware/version/BUILD      — monotonic counter, auto-incremented on every scripted
#                                              build by tools/version-bump.ps1 (so the number always
#                                              moves when you build)
#   g<git-short> short commit hash of HEAD
#   *            present iff the working tree has uncommitted source changes (the version files
#                themselves are EXCLUDED from this check, so the build-counter churn never marks
#                an otherwise-clean tree as dirty)
#
# Must be include()d from firmware/CMakeLists.txt BEFORE the ESP-IDF project.cmake — that is the
# only point where setting PROJECT_VER is honoured (see ESP-IDF "App version" docs). The app
# descriptor field caps at 32 chars; the composition stays well under it and is truncated as a guard.

set(_nv_dir "${CMAKE_CURRENT_LIST_DIR}")          # firmware/version
get_filename_component(_nv_repo "${_nv_dir}/../.." ABSOLUTE)   # repo root

# --- semver + build counter (the two on-disk files) -----------------------------------------------
file(STRINGS "${_nv_dir}/VERSION" _nv_semver LIMIT_COUNT 1)
file(STRINGS "${_nv_dir}/BUILD"   _nv_build  LIMIT_COUNT 1)
string(STRIP "${_nv_semver}" _nv_semver)
string(STRIP "${_nv_build}"  _nv_build)
if("${_nv_semver}" STREQUAL "")
    set(_nv_semver "0.0.0")
endif()
if("${_nv_build}" STREQUAL "")
    set(_nv_build "0")
endif()

# A bumped BUILD (or an edited VERSION) must trigger a reconfigure so the new number lands in the
# app descriptor. CMAKE_CONFIGURE_DEPENDS is the documented hook; skip it in `cmake -P` test mode.
if(NOT CMAKE_SCRIPT_MODE_FILE)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${_nv_dir}/VERSION" "${_nv_dir}/BUILD")
endif()

# --- git short hash + dirty flag ------------------------------------------------------------------
set(_nv_git "nogit")
set(_nv_dirty "")
find_program(_NV_GIT git)
if(_NV_GIT)
    execute_process(
        COMMAND "${_NV_GIT}" rev-parse --short=7 HEAD
        WORKING_DIRECTORY "${_nv_repo}"
        OUTPUT_VARIABLE _nv_git OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET RESULT_VARIABLE _nv_grc)
    if(NOT _nv_grc EQUAL 0 OR "${_nv_git}" STREQUAL "")
        set(_nv_git "nogit")
    endif()
    # Dirty = any uncommitted change OUTSIDE firmware/version (the counter files change every build).
    execute_process(
        COMMAND "${_NV_GIT}" status --porcelain "--" ":(exclude)firmware/version"
        WORKING_DIRECTORY "${_nv_repo}"
        OUTPUT_VARIABLE _nv_st OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(NOT "${_nv_st}" STREQUAL "")
        set(_nv_dirty "*")
    endif()
endif()

# --- compose ---------------------------------------------------------------------------------------
set(PROJECT_VER "${_nv_semver}+${_nv_build}.g${_nv_git}${_nv_dirty}")
string(LENGTH "${PROJECT_VER}" _nv_len)
if(_nv_len GREATER 31)
    string(SUBSTRING "${PROJECT_VER}" 0 31 PROJECT_VER)
endif()
message(STATUS "NucleoOS version (PROJECT_VER): ${PROJECT_VER}")
