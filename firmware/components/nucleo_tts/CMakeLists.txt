idf_component_register(
    SRCS "nucleo_tts.c" "nucleo_tts_plan.c" "nucleo_tts_index.c"
    INCLUDE_DIRS "include"
    REQUIRES nucleo_audio
)
# snprintf in buffer fissi a posta; il troncamento qui e' sicuro.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format-truncation)

# === ANTI-REGRESSIONE PARTE DELLA BUILD ========================================================
# I test della voce non sono piu' un pre-step saltabile: il ctest PURO del planner (tts-plan-ctest.c —
# 71 casi su mathspeak/eq_result/traduttore/decompose/guardia/numeri/orari, ZERO I/O, ~0.5s) viene
# compilato+eseguito DENTRO `idf.py build`. Se la logica testo->clip regredisce, il FIRMWARE NON SI
# COSTRUISCE (esce prima del link del componente). Skip GRAZIOSO se manca un compilatore host: build su
# macchine/CI minimali restano verdi e il gate npm `anima:tts` resta la rete completa (indice/replies/time).
find_program(NUCLEO_HOST_CC NAMES gcc cc
    PATHS C:/msys64/mingw64/bin C:/msys64/ucrt64/bin C:/msys64/clang64/bin)
if(NUCLEO_HOST_CC)
    if(CMAKE_HOST_WIN32)
        set(_tts_exe_sfx ".exe")
    else()
        set(_tts_exe_sfx "")
    endif()
    get_filename_component(_tts_ccdir "${NUCLEO_HOST_CC}" DIRECTORY)    # bin MinGW: serve sul PATH (vedi sotto)
    set(_tts_repo  "${CMAKE_CURRENT_SOURCE_DIR}/../../..")              # nucleo_tts -> components -> firmware -> repo
    set(_tts_plan  "${CMAKE_CURRENT_SOURCE_DIR}/nucleo_tts_plan.c")
    set(_tts_hdr   "${CMAKE_CURRENT_SOURCE_DIR}/include/nucleo_tts.h")
    set(_tts_ctest "${_tts_repo}/tools/anima-host/tts-plan-ctest.c")
    set(_tts_exe   "${CMAKE_CURRENT_BINARY_DIR}/tts_plan_gate${_tts_exe_sfx}")
    set(_tts_stamp "${CMAKE_CURRENT_BINARY_DIR}/tts_plan_gate.passed")
    # CERTEZZA: gcc.exe (e i suoi cc1/as/ld) caricano le DLL MinGW dalla loro stessa cartella -> quella
    # bin DEVE stare sul PATH del processo, altrimenti gcc esce 1 SENZA messaggio (rompeva la build).
    # Il PATH della build IDF non la contiene, quindi avvolgo l'invocazione con `cmake -E env PATH=...`.
    # Uso la SOLA cartella bin MinGW (le DLL di sistema sono trovate comunque): niente ';' nel valore,
    # cosi' CMake non lo spezza in lista (i ';' sono separatori-lista). Validato in shell pulita = 71 ok.
    # -static: l'exe risultante gira poi standalone (nessun DLL a runtime). Stamp + DEPENDS = il gate
    # ri-scatta SOLO quando cambiano il planner o il suo test (altri componenti -> costo zero a regime).
    add_custom_command(
        OUTPUT  "${_tts_stamp}"
        COMMAND "${CMAKE_COMMAND}" -E env "PATH=${_tts_ccdir}"
                "${NUCLEO_HOST_CC}" -std=gnu11 -O0 -static
                -I "${CMAKE_CURRENT_SOURCE_DIR}/include" "${_tts_plan}" "${_tts_ctest}" -o "${_tts_exe}"
        COMMAND "${_tts_exe}"
        COMMAND "${CMAKE_COMMAND}" -E touch "${_tts_stamp}"
        DEPENDS "${_tts_plan}" "${_tts_ctest}" "${_tts_hdr}"
        COMMENT "TTS anti-regressione (ctest planner host) — parte della build"
        VERBATIM)
    add_custom_target(nucleo_tts_anti_regression DEPENDS "${_tts_stamp}")
    add_dependencies(${COMPONENT_LIB} nucleo_tts_anti_regression)
    message(STATUS "nucleo_tts: anti-regressione planner AGGANCIATA alla build (${NUCLEO_HOST_CC})")
else()
    message(STATUS "nucleo_tts: host gcc assente -> build-gate planner SALTATO (rete piena: npm run anima:tts)")
endif()
