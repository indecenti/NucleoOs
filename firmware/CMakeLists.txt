# NucleoOS firmware — ESP-IDF top-level project file.
cmake_minimum_required(VERSION 3.16)
# All components build under ESP-IDF: nucleo_app (the Wear OS-style launcher) was ported
# off the Arduino M5Cardputer lib to M5GFX + nucleo_kbd, so it is no longer excluded.
# main.c REQUIRES and calls it (nucleo_app_register_builtins / nucleo_app_run), so leaving
# it excluded breaks a clean configure. Changing EXCLUDE_COMPONENTS needs a reconfigure
# (run `idf.py fullclean` once after editing this line).
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(nucleoos)
