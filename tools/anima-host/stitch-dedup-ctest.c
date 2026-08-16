// MOSAICO SPAN-DEDUP unit test — proves the L2 span-stitch never says the same thing twice.
//
// Why a unit test and not only an end-to-end sweep: the defect (a drill-down span that RESTATES the
// lead in different words, so the stitched answer repeats itself) depends on which two cards the index
// happens to pair. On the current host fixture no pair triggers it, so an exe-level sweep would report
// "0 changed" and prove nothing. Here we drive the guard directly with the shapes that caused it.
//
// The helpers under test are `static` in nucleo_anima_l1.c, so we #include the REAL translation unit
// instead of copying the code — this test can never drift from what the device runs. The runner
// (stitch-dedup.mjs) links it against the same host sources as anima.exe, minus that file and main.
#include "nucleo_anima_l1.c"

#include <stdio.h>

static int failures = 0;

static void check_span(const char *what, const char *buf, const char *span, const char *want)
{
    char out[400];
    bool got_any = l1_span_new(out, sizeof out, buf, span);
    const char *got = got_any ? out : "";
    if (strcmp(got, want) != 0) {
        failures++;
        printf("  FAIL %s\n    buf : %s\n    span: %s\n    want: \"%s\"\n    got : \"%s\"\n",
               what, buf, span, want, got);
    } else {
        printf("  ok   %s\n", what);
    }
}

static void check_lang(const char *what, const char *lead, const char *span, bool want)
{
    bool got = l1_lang_agrees(lead, span);
    if (got != want) {
        failures++;
        printf("  FAIL %s  (want %s, got %s)\n    lead: %s\n    span: %s\n",
               what, want ? "agree" : "disagree", got ? "agree" : "disagree", lead, span);
    } else {
        printf("  ok   %s\n", what);
    }
}

static void check_redundant(const char *what, const char *buf, const char *sent, bool want)
{
    char have[512];
    l1_norm_words(have, sizeof have, buf);
    bool got = l1_sent_redundant(have, sent);
    if (got != want) {
        failures++;
        printf("  FAIL %s  (want %s, got %s)\n    buf : %s\n    sent: %s\n",
               what, want ? "redundant" : "new", got ? "redundant" : "new", buf, sent);
    } else {
        printf("  ok   %s\n", what);
    }
}

int main(void)
{
    const char *lead = "NucleoOS e un sistema operativo offline per il Cardputer.";

    printf("[stitch-dedup] sentence-level redundancy\n");
    // The bug as reported: the second span repeats the lead's claim in other words. Word-for-word
    // containment misses it and l1_head_in only ever looked at the first ~40 chars.
    check_redundant("reworded restatement is redundant", lead,
                    "Il Cardputer usa NucleoOS, un sistema operativo offline.", true);
    check_redundant("verbatim restatement is redundant", lead, lead, true);
    check_redundant("genuinely new sentence is kept", lead,
                    "Include una shell con comandi e l'assistente ANIMA integrato.", false);
    // Too short to judge: dropping these would lose real content on a terse card, so they pass through.
    check_redundant("very short sentence is kept", lead, "E offline.", false);
    // Accented text must compare consistently (both sides go through the same normalizer).
    check_redundant("accents don't break the compare",
                    "La fotosintesi e il processo con cui le piante producono zuccheri.",
                    "Le piante producono zuccheri con il processo della fotosintesi.", true);

    printf("[stitch-dedup] span filtering\n");
    check_span("fully redundant span is dropped", lead,
               "NucleoOS e un sistema operativo offline per il Cardputer.", "");
    check_span("fully new span is kept whole", lead,
               "Include una shell con comandi e l'assistente ANIMA integrato.",
               "Include una shell con comandi e l'assistente ANIMA integrato.");
    // The real MOSAICO shape: a drill-down whose first sentence restates the lead and whose second
    // one actually adds something. Only the second may be stapled on.
    check_span("mixed span keeps only the new sentence", lead,
               "NucleoOS e un sistema operativo offline per il Cardputer. "
               "La shell accetta comandi digitati dalla tastiera.",
               "La shell accetta comandi digitati dalla tastiera.");
    // A span must not duplicate ITSELF either — kept sentences are folded back into the comparison,
    // so the second sentence here is measured against the FIRST one (which the lead never mentioned).
    check_span("span does not duplicate itself", lead,
               "La shell accetta comandi digitati dalla tastiera. "
               "La shell accetta i comandi digitati dalla tastiera.",
               "La shell accetta comandi digitati dalla tastiera.");
    // ...but a second sentence that only PARTLY overlaps is real added detail and must survive: the
    // guard drops restatements, not elaborations.
    check_span("partial overlap is not a duplicate", lead,
               "La shell accetta comandi digitati dalla tastiera. "
               "I comandi digitati sulla tastiera arrivano alla shell.",
               "La shell accetta comandi digitati dalla tastiera. "
               "I comandi digitati sulla tastiera arrivano alla shell.");
    // Nothing to add and nothing to compare against are both handled without a crash.
    check_span("empty span yields nothing", lead, "", "");
    check_span("empty lead keeps the span", "", "Include una shell con comandi.",
               "Include una shell con comandi.");

    // The real case: "what is force" answered with an Italian lead, then stapled an English runner-up
    // on with " Also, ". Two correct cards, one unreadable bilingual paragraph.
    printf("[stitch-dedup] language agreement\n");
    check_lang("IT lead rejects EN span",
               "In meccanica la forza e una grandezza fisica vettoriale in grado di indurre una variazione "
               "dello stato di quiete o di moto di un corpo.",
               "A force is a push or pull that can change an object's motion.", false);
    check_lang("IT lead accepts IT span",
               "In meccanica la forza e una grandezza fisica vettoriale in grado di indurre una variazione "
               "dello stato di quiete o di moto di un corpo.",
               "In presenza di piu forze, l'effetto e determinato dalla risultante della loro composizione.", true);
    check_lang("EN lead accepts EN span",
               "Energy is the capacity to do work; it transforms but is never created or destroyed.",
               "It exists in many forms and converts between them, as when a battery turns chemical into electrical.", true);
    check_lang("EN lead rejects IT span",
               "Energy is the capacity to do work; it transforms but is never created or destroyed.",
               "L'energia e la grandezza fisica che misura la capacita di un corpo di compiere un lavoro.", false);
    // Permissive by design: an undecidable span must pass, so the guard can only ever remove a
    // genuinely bilingual paragraph — never a terse one whose language can't be told.
    check_lang("undecidable span is allowed through",
               "Energy is the capacity to do work; it transforms but is never created or destroyed.",
               "E = mc2.", true);

    printf(failures ? "[stitch-dedup] %d FAILED\n" : "[stitch-dedup] all checks passed\n", failures);
    return failures ? 1 : 0;
}
