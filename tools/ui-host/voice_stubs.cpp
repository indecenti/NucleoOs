// Host stubs for the Voice app gallery: a small app registry for the "open with" picker, and one bound word.
#include <string.h>
#include <stdbool.h>
#include <stddef.h>

static const struct { const char *id, *name; } kApps[] = {
    { "voice", "Voice" }, { "anima", "ANIMA" }, { "music", "Music" }, { "clock", "Clock" }, { "torch", "Torch" },
    { "calc", "Calculator" }, { "weather", "Weather" }, { "recorder", "Recorder" }, { "photos", "Photos" },
};

int nucleo_app_count(void) { return (int)(sizeof kApps / sizeof kApps[0]); }
extern "C" bool nucleo_app_info_at(int i, const char **id, const char **title)
{
    if (i < 0 || i >= nucleo_app_count()) return false;
    if (id) *id = kApps[i].id;
    if (title) *title = kApps[i].name;
    return true;
}
extern "C" const char *launcher_app_localized_name(const char *) { return nullptr; }

extern "C" bool nucleo_voice_binding_get(const char *word, char *app, size_t cap)
{
    if (app && cap) app[0] = 0;
    if (word && !strcmp(word, "anima") && app && cap > 5) { strcpy(app, "anima"); return true; }
    if (word && !strcmp(word, "torcia") && app && cap > 5) { strcpy(app, "torch"); return true; }
    return false;
}
extern "C" int nucleo_voice_binding_set(const char *, const char *) { return 0; }
extern "C" void nucleo_voice_set_bound_only(bool) {}
