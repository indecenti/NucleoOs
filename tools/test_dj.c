#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../firmware/components/nucleo_audio/include/nucleo_npx.h"
#include "../firmware/components/nucleo_audio/include/nucleo_dj_planner.h"

// Stubs for npx_next_beat, npx_beat_ms, npx_drop_s since we don't compile nucleo_npx.c
// to keep the test strictly isolated from ESP-IDF.

float npx_beat_ms(const NpxData *npx) {
    if (npx->hdr.bpm > 0) return 60000.0f / npx->hdr.bpm;
    return 0.0f;
}

float npx_next_beat(const NpxData *npx, float pos_s) {
    if (npx->hdr.bpm > 0) {
        float period = 60.0f / npx->hdr.bpm;
        // Mock a beat grid starting at 0.0
        int k = (int)(pos_s / period);
        float beat = k * period;
        while (beat < pos_s) beat += period;
        return beat;
    }
    return -1.0f;
}

float npx_drop_s(const NpxData *npx) {
    // We can mock this by looking at a custom field or just returning a fixed value.
    // Let's assume the drop is always at 30.0s for test purposes, if bpm > 0.
    return (npx->hdr.bpm > 0) ? 30.0f : -1.0f;
}

int main() {
    printf("Testing DJ Planner C implementation...\n");

    NpxData npxA = {0};
    npxA.hdr.bpm = 128.0f;
    DjTrackMeta metaA = { "electronic", 3, 180.0f };

    NpxData npxB = {0};
    npxB.hdr.bpm = 130.0f;
    DjTrackMeta metaB = { "electronic", 3, 200.0f };

    DjPlan plan;
    
    // Test 1: Beatmatch Club tracks
    printf("\n[Test 1] Electronic 128 BPM -> Electronic 130 BPM\n");
    dj_planner_plan(&npxA, &metaA, &npxB, &metaB, &plan);
    printf("Type: %d (0=BLEND, 1=SLAM, 2=FADE)\n", plan.type);
    printf("Beat Align: %s\n", plan.beat_align ? "Yes" : "No");
    printf("Cut A at: %.2f s\n", plan.a_out_time_s);
    printf("Start B at: %.2f s\n", plan.b_in_time_s);
    printf("Fade: %d ms\n", plan.fade_ms);

    // Test 2: Different families, FADE
    metaA.family = "pop"; metaA.energy = 2; npxA.hdr.bpm = 110.0f;
    metaB.family = "rock"; metaB.energy = 2; npxB.hdr.bpm = 140.0f;
    printf("\n[Test 2] Pop 110 BPM -> Rock 140 BPM\n");
    dj_planner_plan(&npxA, &metaA, &npxB, &metaB, &plan);
    printf("Type: %d\n", plan.type);
    printf("Beat Align: %s\n", plan.beat_align ? "Yes" : "No");
    printf("Cut A at: %.2f s\n", plan.a_out_time_s);
    printf("Start B at: %.2f s\n", plan.b_in_time_s);
    printf("Fade: %d ms\n", plan.fade_ms);

    // Test 3: Hard vs Hard (beatmatched slam)
    metaA.family = "hard"; metaA.energy = 3; npxA.hdr.bpm = 150.0f;
    metaB.family = "hard"; metaB.energy = 3; npxB.hdr.bpm = 155.0f;
    printf("\n[Test 3] Hard 150 BPM -> Hard 155 BPM\n");
    dj_planner_plan(&npxA, &metaA, &npxB, &metaB, &plan);
    printf("Type: %d\n", plan.type);
    printf("Beat Align: %s\n", plan.beat_align ? "Yes" : "No");
    printf("Cut A at: %.2f s\n", plan.a_out_time_s);
    printf("Start B at: %.2f s\n", plan.b_in_time_s);

    return 0;
}
