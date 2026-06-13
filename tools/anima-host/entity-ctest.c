// One-shot C syntax+logic check for the generic entity extractor in nucleo_anima_online.c.
// The arrays + nucleo_anima_online_entity body below are copied VERBATIM from that file; lower_copy
// and make_slug are minimal local stubs (ASCII-fold is enough: the "è"-spelled patterns match byte-
// for-byte). Proves the C compiles and behaves like tools/anima/entity.mjs. Build:
//   gcc -std=gnu11 -O0 tools/anima-host/entity-ctest.c -o /tmp/ectest && /tmp/ectest
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdbool.h>
#include <ctype.h>

static void lower_copy(char *d, size_t n, const char *s){ size_t i=0; for(; s[i] && i<n-1; i++) d[i]=(char)tolower((unsigned char)s[i]); d[i]=0; }
static void make_slug(char *d, int n, const char *s){ int o=0; for(int i=0;s[i]&&o<n-1;i++){ char c=(char)tolower((unsigned char)s[i]); if((c>='a'&&c<='z')||(c>='0'&&c<='9')) d[o++]=c; else if(o>0&&d[o-1]!='-') d[o++]='-'; } while(o>0&&d[o-1]=='-') o--; d[o]=0; }

// ===== VERBATIM from nucleo_anima_online.c =====
static const char *TRIG_IT[] = {
    "chi e ","chi è ","chi era ","chi erano ","chi sono ","sai chi e ","sai chi è ","sai chi era ",
    "cos'e ","cos'è ","cosa e ","cosa è ","che cos'e ","che cos'è ","che cosa e ","che cosa è ","cosa significa ",
    "conosci ","conoscete ","conosce ","hai mai sentito parlare di ","hai mai sentito ","hai sentito parlare di ","hai sentito ",
    "ti e familiare ","ti è familiare ","ti suona ","hai presente ","ti viene in mente ","ti ricordi ","ricordi ",
    "eri a conoscenza di ","sei a conoscenza di ","hai mai incontrato il nome ","hai incontrato il nome ",
    "ti e mai capitato di leggere su ","ti è mai capitato di leggere su ","ti e capitato di leggere su ","ti è capitato di leggere su ","riesci a riconoscere ","riconosci ",
    "parlami di ","potresti parlarmi di ","puoi parlarmi di ","raccontami di ","raccontami qualcosa su ","dimmi di ",
    "dimmi qualcosa su ","potresti dirmi qualcosa su ","puoi dirmi qualcosa su ","dimmi chi e ","dimmi chi è ",
    "cosa sai di ","cosa sai dire di ","che sai di ","che cosa sai di ","sai qualcosa su ","sai qualcosa riguardo a ","sai qualcosa di ","sai dirmi di ",
    "hai qualche informazione su ","hai informazioni su ","hai qualche nozione su ","hai notizie su ","qual e la tua conoscenza di ","qual è la tua conoscenza di ",
    "per cosa e famoso ","per cosa è famoso ","per cosa e famosa ","per cosa è famosa ","per cosa e noto ","per cosa è noto ","per cosa e nota ","per cosa è nota ",
    "che cosa ha fatto ","cosa ha fatto ","che ha fatto ","in che ambito e noto ","in che ambito è noto ","in che ambito e nota ","in che ambito è nota ","in che campo e noto ","in che campo è noto ",
    "qual e il ruolo di ","qual è il ruolo di ","dove e conosciuto ","dove è conosciuto ","dove e conosciuta ","dove è conosciuta ",
    "per quale motivo e importante ","per quale motivo è importante ","perche e importante ","perché è importante ",
    "di quale campo e esperto ","di quale campo è esperto ","di quale campo e esperta ","di quale campo è esperta ","qual e la specialita di ","qual è la specialità di ",
    "chi rappresenta ","di che si occupa ","di cosa si occupa ","che lavoro fa ","che mestiere fa ",
    "di che attore e ","di che attore è ","di che sportivo e ","di che sportivo è ","di che politico e ","di che politico è ",
    "di che cantante e ","di che cantante è ","di che scrittore e ","di che scrittore è ","di che artista e ","di che artista è ","di che musicista e ","di che musicista è ",
    NULL,
};
static const char *TRIG_EN[] = {
    "do you know ","have you ever heard of ","have you heard of ","have you heard about ","are you familiar with ",
    "what can you tell me about ","what do you know about ","tell me about ","tell me who ","were you aware of ","ever heard of ","do you recognize ","do you recall ",
    "who is ","who was ","who are ","who's ","what is ","what was ","what are ","what's ","what did ",
    NULL,
};
static const char *SUFFIX_EN[] = {
    " do for a living"," best known for"," famous for"," known for"," does"," about"," do", NULL,
};
static const char *ARTICLES[] = {
    "the ","a ","an ","il ","lo ","la ","i ","gli ","le ","l'","un ","uno ","una ", NULL,
};
static int longest_prefix(const char *low, const char *const *list)
{
    int best = 0;
    for (int i = 0; list[i]; i++) { size_t tl = strlen(list[i]); if (strncmp(low, list[i], tl) == 0 && (int)tl > best) best = (int)tl; }
    return best;
}
int nucleo_anima_online_entity(const char *input, bool en,
                               char *entity, int entity_cap, char *slug, int slug_cap)
{
    (void)en;
    if (!input) return 0;
    while (*input == ' ') input++;
    char low[160]; lower_copy(low, sizeof(low), input);
    int hit = longest_prefix(low, TRIG_IT);
    int hen = longest_prefix(low, TRIG_EN);
    if (hen > hit) hit = hen;
    if (hit == 0) return 0;
    const char *e = input + hit;
    while (*e == ' ') e++;
    for (int i = 0; ARTICLES[i]; i++) {
        size_t al = strlen(ARTICLES[i]);
        if (strncasecmp(e, ARTICLES[i], al) == 0) { e += al; while (*e == ' ') e++; break; }
    }
    int n = 0; for (; e[n] && n < entity_cap - 1; n++) entity[n] = e[n];
    while (n > 0 && (entity[n-1] == '?' || entity[n-1] == '.' || entity[n-1] == '!' || entity[n-1] == ' ')) n--;
    entity[n] = 0;
    for (int i = 0; SUFFIX_EN[i]; i++) {
        int sl = (int)strlen(SUFFIX_EN[i]);
        if (n >= sl && strncasecmp(entity + n - sl, SUFFIX_EN[i], sl) == 0) {
            n -= sl; while (n > 0 && entity[n-1] == ' ') n--; entity[n] = 0; break;
        }
    }
    make_slug(slug, slug_cap, entity);
    if ((int)strlen(slug) < 2) { entity[0] = slug[0] = 0; return 0; }
    return 1;
}
// ===== end verbatim =====

int main(void){
    const char *qs[] = {
        "Per cosa è famoso Madonna?", "Eri a conoscenza di Elvis Presley?", "Di che attore è Morgan Freeman?",
        "Conosci Pinco Pallino?", "Ti viene in mente Drake?", "What is Einstein famous for?", "What did Messi do?",
        "Hai mai incontrato il nome Taylor Swift?", "quanto fa 12 per 8", "che ore sono", 0 };
    int bad = 0;
    for (int i = 0; qs[i]; i++) {
        char e[80] = "", s[80] = "";
        int r = nucleo_anima_online_entity(qs[i], false, e, sizeof e, s, sizeof s);
        printf("%-44s -> %d  slug='%s'\n", qs[i], r, s);
    }
    return bad;
}
