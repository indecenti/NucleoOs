#!/usr/bin/env python3
"""Bake the most globally famous people into ANIMA's offline corpus (knowledge/people.jsonl).

Source: reliable Wikimedia REST (NOT the WDQS SPARQL endpoint, which is rate-limited/outage-prone) —
  1) Pageviews "top" API: the most-VIEWED Wikipedia articles per month = a real global-fame signal.
  2) REST summary API: per article, the Wikidata description + the lead `extract`, in IT and EN.
We aggregate views over several months, rank by total views, keep the ones whose description says
"person" (a profession/role or a lifespan), and turn each into a curated bilingual card with dense
`ask` phrasings from ANIMA's own entity grammar. RAM-free (cards live in the SD index), persistent.

Run:  python tools/anima/gen_people.py   then  python tools/anima/build_akb2.py
"""
import json, os, re, sys, time, unicodedata, urllib.request, urllib.parse

UA = "NucleoOS-ANIMA/1.0 (offline-assistant corpus builder; +https://github.com/nucleoos)"
TARGET = 1000
MONTHS = ["2024/01","2024/04","2024/07","2024/10","2025/01","2025/04","2025/07","2025/10","2023/06","2023/12"]
MAX_CANDIDATES = 3500

def get(url, retries=3):
    for a in range(retries):
        try:
            with urllib.request.urlopen(urllib.request.Request(url, headers={"User-Agent": UA}), timeout=30) as r:
                return json.load(r)
        except Exception:
            time.sleep(0.5 * (a + 1))
    return None

def top_articles():
    agg = {}
    for m in MONTHS:
        d = get(f"https://wikimedia.org/api/rest_v1/metrics/pageviews/top/en.wikipedia/all-access/{m}/all-days")
        if not d: print(f"[people] no pageviews for {m}", file=sys.stderr); continue
        for a in d["items"][0]["articles"]:
            t = a["article"]
            if ":" in t or t.startswith("List_of") or t == "Main_Page" or t.endswith("_(disambiguation)"): continue
            agg[t] = agg.get(t, 0) + int(a["views"])
        print(f"[people] {m}: {len(agg)} unique titles so far", file=sys.stderr)
        time.sleep(0.2)
    return [t for t, _ in sorted(agg.items(), key=lambda kv: -kv[1])]

REJECT = ("film","song","album","single by","ep by","series","tv ","television","city","town","village",
  "capital of","municipality","novel","band","musical group","company","video game","manga","anime",
  "river","mountain","lake","island","country","language","software","website","sports team","football club",
  "league","championship","tournament","competition","festival","holiday"," war","battle","building",
  "neighborhood","district"," region","province","dynasty","empire","wikipedia","franchise","fictional",
  "comics","genus","species","operating system","soundtrack","web series","miniseries","poem","magazine",
  "newspaper","video game","play by","painting","sculpture","enterprise","organization","political party")
ROLE = ("singer","actor","actress","rapper","musician","songwriter","composer","guitarist","drummer","dj",
  "politician","president","prime minister","king","queen","emperor","empress","dictator","statesman",
  "writer","author","novelist","poet","playwright","journalist","philosopher","historian","theologian",
  "scientist","physicist","chemist","biologist","mathematician","engineer","inventor","astronaut","astronomer",
  "footballer","player","athlete","boxer","cyclist","sprinter","swimmer","wrestler","golfer","racing driver","coach",
  "painter","artist","sculptor","architect","designer","film director","producer","screenwriter","comedian",
  "entrepreneur","businessman","businesswoman","investor","economist","activist","revolutionary","monarch",
  "model","presenter","youtuber","chef","explorer","general","admiral","pope","saint","psychologist","lawyer",
  "cantante","attore","attrice","politico","scrittore","calciatore","cestista","tennista","pittore","regista","rapper")

def is_person(desc):
    d = (desc or "").lower().strip()
    if not d: return False
    if any(w in d for w in REJECT): return False
    if re.search(r"\(\s*\d{3,4}", d) or re.search(r"\bborn \d{4}\b", d) or re.search(r"\bnat[oa] nel \d{4}\b", d): return True
    return any(w in d for w in ROLE)

def first_sentence(t, cap=240):
    t = " ".join((t or "").split())
    m = re.search(r"(?<=[.!?])\s", t)
    s = t[:m.start()+1] if m else t
    if len(s) > cap: s = s[:cap].rsplit(" ", 1)[0] + "…"
    return s

def slugify(s):
    s = "".join(c for c in unicodedata.normalize("NFD", s.lower()) if unicodedata.category(c) != "Mn")
    return re.sub(r"[^a-z0-9]+", "-", s).strip("-")

def summary(proj, title):
    return get(f"https://{proj}.wikipedia.org/api/rest_v1/page/summary/{urllib.parse.quote(title)}")

# Must-have icons (the user's own list) — pageviews are event-driven and miss evergreen-famous people
# (Kurt Cobain, Freddie Mercury). These are SEEDED explicitly so they're always present. A few need a
# disambiguated title (the bare name is a disambiguation page).
OVERRIDE = {"Madonna": "Madonna (entertainer)", "Prince": "Prince (musician)", "Drake": "Drake (musician)",
            "Martin Luther King": "Martin Luther King Jr."}
SEED = ("Kurt Cobain;Madonna;Michael Jackson;Bob Marley;Prince;David Bowie;Elvis Presley;Johnny Cash;"
  "Freddie Mercury;Adele;Beyoncé;Elton John;Lady Gaga;Taylor Swift;Bruno Mars;Eminem;Jay-Z;Kanye West;"
  "Drake;Rihanna;Tom Hanks;Morgan Freeman;Leonardo DiCaprio;Brad Pitt;Robert De Niro;Al Pacino;"
  "Sean Connery;Denzel Washington;Will Smith;Christian Bale;Joaquin Phoenix;Natalie Portman;Angelina Jolie;"
  "Scarlett Johansson;Emma Stone;Jennifer Lawrence;Meryl Streep;Cate Blanchett;Helen Mirren;Julia Roberts;"
  "Donald Trump;Joe Biden;Barack Obama;John F. Kennedy;Nelson Mandela;Mahatma Gandhi;Martin Luther King;"
  "Winston Churchill;Adolf Hitler;Joseph Stalin;Fidel Castro;Che Guevara;Vladimir Putin;Xi Jinping;"
  "Lula da Silva;Volodymyr Zelensky;Angela Merkel;Emmanuel Macron;Boris Johnson;William Shakespeare;"
  "Mark Twain;Ernest Hemingway;F. Scott Fitzgerald;J. K. Rowling;George Orwell;Jane Austen;Charles Dickens;"
  "Leo Tolstoy;Franz Kafka;Friedrich Nietzsche;Oscar Wilde;Emily Dickinson;Virginia Woolf;"
  "Gabriel García Márquez;Pablo Neruda;Dante Alighieri;Giovanni Boccaccio;Francesco Petrarca;"
  "Alessandro Manzoni;Michael Jordan;LeBron James;Tom Brady;Lionel Messi;Cristiano Ronaldo;Pelé;"
  "David Beckham;Zlatan Ibrahimović;Roger Federer;Rafael Nadal;Serena Williams;Usain Bolt;Muhammad Ali;"
  "Kobe Bryant;Tiger Woods;Babe Ruth;Albert Einstein;Isaac Newton;Charles Darwin;Stephen Hawking;"
  "Leonardo da Vinci;Pablo Picasso;Vincent van Gogh;Wolfgang Amadeus Mozart;Ludwig van Beethoven;"
  "Steve Jobs;Bill Gates;Elon Musk;Marie Curie;Napoleon;Cleopatra;Julius Caesar;Abraham Lincoln").split(";")

DISAMBIG = ("may refer to", "puè riferirsi a", "può riferirsi a", "pagina di disambiguazione", "disambiguation page")

def build_card(title, force=False):
    en = summary("en", title); time.sleep(0.03)
    if not en or en.get("type") == "disambiguation": return None
    if any(m in (en.get("extract") or "").lower() for m in DISAMBIG): return None
    if not force and not is_person(en.get("description")): return None
    name_en = en.get("title") or title.replace("_", " ")
    it = summary("it", title); time.sleep(0.03)
    if it and (it.get("type") == "disambiguation" or any(m in (it.get("extract") or "").lower() for m in DISAMBIG)): it = None
    name_it = (it or {}).get("title") or name_en
    rit = first_sentence((it or {}).get("extract")) or f"{name_it}: {(it or {}).get('description') or en.get('description')}."
    ren = first_sentence(en.get("extract")) or f"{name_en}: {en.get('description')}."
    if not rit or not ren: return None
    return {
        "id": f"person.{slugify(name_en)}", "category": "person", "action": "answer", "arg": "",
        "reply": {"it": rit[:250], "en": ren[:250]},
        "ask": {"it": [t.format(n=name_it) for t in T_IT], "en": [t.format(n=name_en) for t in T_EN]},
        "source": "wikipedia", "lang_primary": "bi", "tags": ["person"],
    }

T_IT = ["chi è {n}", "chi era {n}", "conosci {n}", "cosa sai di {n}", "parlami di {n}",
        "per cosa è famoso {n}", "che cosa ha fatto {n}", "hai mai sentito {n}", "{n}", "{n} chi è"]
T_EN = ["who is {n}", "who was {n}", "do you know {n}", "tell me about {n}", "what is {n} famous for", "{n}"]

def main():
    seen, cards = set(), []
    def add(card):
        if not card: return
        slug = card["id"][len("person."):]
        if len(slug) < 2 or slug in seen: return
        seen.add(slug); cards.append(card)
    print(f"[people] seeding {len(SEED)} must-have icons…", file=sys.stderr)
    for name in SEED:
        add(build_card(OVERRIDE.get(name, name), force=True))
    print(f"[people] {len(cards)}/{len(SEED)} seeds kept; topping up from pageviews…", file=sys.stderr)
    cands = top_articles()[:MAX_CANDIDATES]
    for i, title in enumerate(cands):
        if len(cards) >= TARGET: break
        n0 = len(cards); add(build_card(title))
        if len(cards) != n0 and len(cards) % 100 == 0: print(f"[people] {len(cards)} people (scanned {i+1})", file=sys.stderr)
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "knowledge", "people.jsonl")
    with open(out, "w", encoding="utf-8") as f:
        for c in cards: f.write(json.dumps(c, ensure_ascii=False) + "\n")
    print(f"[people] wrote {len(cards)} people -> {out}")

if __name__ == "__main__":
    main()
