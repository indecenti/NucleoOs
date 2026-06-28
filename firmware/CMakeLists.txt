# NucleoOS firmware — ESP-IDF top-level project file.
cmake_minimum_required(VERSION 3.16)
# All components build under ESP-IDF: nucleo_app (the Wear OS-style launcher) was ported
# off the Arduino M5Cardputer lib to M5GFX + nucleo_kbd, so it is no longer excluded.
# main.c REQUIRES and calls it (nucleo_app_register_builtins / nucleo_app_run), so leaving
# it excluded breaks a clean configure. Changing EXCLUDE_COMPONENTS needs a reconfigure
# (run `idf.py fullclean` once after editing this line).
# NucleoOS version -> PROJECT_VER -> baked into the app descriptor (esp_app_get_description()).
# MUST run before project.cmake: that's the only point where PROJECT_VER is honoured. Single
# source of truth lives in firmware/version/ (see firmware/version/version.cmake).
include("${CMAKE_CURRENT_LIST_DIR}/version/version.cmake")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(nucleoos)
