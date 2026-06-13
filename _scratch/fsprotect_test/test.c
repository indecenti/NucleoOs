// Host unit test for nucleo_fs_is_protected(). Build: see run command below.
#include <stdio.h>
#include "nucleo_fsprotect.h"

static int fails = 0, total = 0;
static void check(const char *path, bool want)
{
    total++;
    bool got = nucleo_fs_is_protected(path);
    if (got != want) {
        fails++;
        printf("  FAIL  %-44s expected %s got %s\n", path,
               want ? "PROTECT" : "free", got ? "PROTECT" : "free");
    }
}

int main(void)
{
    // ---- PROTECTED: system trees ----
    check("/sd/apps/anima/www/index.html", true);
    check("/sd/apps", true);
    check("/sd/apps/", true);
    check("/sd/www/shell/index.html.gz", true);
    check("/sd/www", true);
    check("/sd/system/registry/apps.json", true);
    check("/sd/system/registry", true);
    check("/sd/system/web/archived.html", true);
    check("/sd/system", true);

    // ---- PROTECTED: ANIMA brain under /data/anima ----
    check("/sd/data/anima/anima-it-encoder.bin", true);
    check("/sd/data/anima/anima-it-encoder.json", true);
    check("/sd/data/anima/anima-it-index.bin", true);
    check("/sd/data/anima/anima-it-index.bin.prov", true);
    check("/sd/data/anima/anima-it-akb5.bin", true);
    check("/sd/data/anima/dict-it-en.tsv", true);
    check("/sd/data/anima/dict-en-it.tsv", true);
    check("/sd/data/anima/commands.it.json", true);
    check("/sd/data/anima/akb5/ai-ml.bin", true);
    check("/sd/data/anima/akb5", true);

    // ---- BYPASS attempts must still be caught ----
    check("/sd/APPS/anima/manifest.json", true);          // case
    check("/sd/Apps", true);                               // case
    check("/sd//apps//anima", true);                       // double slash
    check("/sd/apps/./anima", true);                       // dot segment
    check("/sd/DATA/ANIMA/AKB5/ART-MUSIC.BIN", true);      // case throughout
    check("/sd\\apps\\anima", true);                        // backslashes
    check("/apps/x", true);                                 // tolerant: no /sd prefix

    // ---- FREE: user data anywhere under /data (except the brain) ----
    check("/sd/data/Music/song.mp3", false);
    check("/sd/data/Videos/clip.mp4", false);
    check("/sd/data/Documents/notes.txt", false);
    check("/sd/data/Recordings/take1.wav", false);
    check("/sd/data/downloads/file.zip", false);
    check("/sd/data/ROMs/game.gb", false);
    check("/sd/data", false);                              // the data dir itself

    // ---- FREE: ANIMA user state (regenerable / user-owned) ----
    check("/sd/data/anima", false);                       // dir node (children guard themselves)
    check("/sd/data/anima/learned/card42.json", false);
    check("/sd/data/anima/learned", false);
    check("/sd/data/anima/teacher.json", false);
    check("/sd/data/anima/sessions.json", false);
    check("/sd/data/anima/session.txt", false);
    check("/sd/data/anima/telemetry.ndjson", false);
    check("/sd/data/anima/workspace.json", false);
    check("/sd/data/anima/profile.tsv", false);
    check("/sd/data/anima/learned.vec", false);
    check("/sd/data/anima/online-cache.jsonl", false);

    // ---- FREE: device state under /system ----
    check("/sd/system/config/settings.json", false);
    check("/sd/system/config", false);
    check("/sd/system/keys/auth.json", false);
    check("/sd/system/sessions/s1.json", false);
    check("/sd/system/logs/boot.log", false);
    check("/sd/system/log/x", false);

    // ---- FREE: non-essential / user-curated / root state ----
    check("/sd/wallpapers/default.jpg", false);
    check("/sd/evilportal/portals/clone.html", false);
    check("/sd/config/volume.json", false);
    check("/sd/backups/2026.zip", false);
    check("/sd/journal/events.ndjson", false);
    check("/sd/README.md", false);
    check("/sd/.deploy-manifest.json", false);

    // ---- prefix-collision guards (must NOT over-match) ----
    check("/sd/data/animals/pet.txt", false);             // not data/anima
    check("/sd/data/anima/akb5extra/x.bin", false);       // not the akb5 dir
    check("/sd/appstore/thing", false);                   // not /apps
    check("/sd/systemic/x", false);                       // not /system

    // ---- degenerate ----
    check("/sd", false);
    check("/sd/", false);
    check("", false);

    printf("\n%s  %d/%d passed (%d failed)\n",
           fails ? "RESULT: FAIL" : "RESULT: OK", total - fails, total, fails);
    return fails ? 1 : 0;
}
