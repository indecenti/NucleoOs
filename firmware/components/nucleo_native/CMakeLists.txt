# Native app loader, portable half (validation + relocation of .nap modules). No ESP-IDF dependency: the
# device glue lives in nucleo_app (native_apps.cpp + the generated ABI export table nap_exports.S), the host
# gate compiles this same file (npm run native:test). Spec: docs/native-apps.md.
idf_component_register(
    SRCS "nap_loader.c"
    INCLUDE_DIRS "include" "../../../sdk/native/include"
)
