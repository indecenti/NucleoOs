#!/usr/bin/env python3
"""Rigorous A/B for the L0 hardening: same binary, L0_LEGACY=1 (before) vs normal (after), at the REAL
device gate. Diffs every in-scope query's (tier,reply) and every junk query's answered/refused, so a
regression (a correct answer turned into none/wrong) cannot hide. Zero assumptions about counts.
"""
import os, sys, subprocess, re, json
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import anima_lib as A
HERE = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.dirname(os.path.dirname(HERE))

def nk(s):
    s=(s or '').lower(); s=''.join(c if ('a'<=c<='z' or '0'<=c<='9' or c==' ') else ' ' for c in s); return ' '.join(s.split())[:48]
cards,_=A.load_corpus(); r2c={}
for c in cards:
    for lg in ('it','en'):
        if nk(c.get('reply',{}).get(lg) or ''): r2c.setdefault(nk(c.get('reply',{}).get(lg)), c['id'])
def matches(expect, cid):
    if cid is None: return False
    if isinstance(expect,list): return any(matches(e,cid) for e in expect)
    if expect.endswith('*'): return cid.startswith(expect[:-1])
    return cid==expect

ins=[]; junk=[]
for l in open(os.path.join(HERE,'eval_ood.jsonl'),encoding='utf-8'):
    l=l.strip()
    if not l or l.startswith('//'): continue
    o=json.loads(l); (junk if o['expect']=='none' else ins).append(o)
for l in open(os.path.join(HERE,'.bigjunk.txt'),encoding='utf-8'):
    if l.strip(): junk.append({'q':l.strip(),'expect':'none'})

allq=[x['q'] for x in ins]+[x['q'] for x in junk]
def run(legacy):
    open(os.path.join(HERE,'.ab_q.txt'),'w',encoding='utf-8').write("\n".join(allq)+"\n")
    env=dict(os.environ);
    if legacy: env['L0_LEGACY']='1'
    out=subprocess.run(['node',os.path.join(ROOT,'tools','anima-host','anima.mjs'),'--file',os.path.join(HERE,'.ab_q.txt')],
                       capture_output=True,encoding='utf-8',errors='replace',env=env).stdout
    rows={}
    for b in re.split(r'\nQ: ',out):
        q=b.split('\n',1)[0].strip()
        tier=(re.search(r'tier=(\S+)',b) or [None,'?'])[1]; rep=(re.search(r'reply:\s*(.*)',b) or [None,''])[1].strip()
        rows[q]={'tier':tier,'rep':rep}
    return rows
before=run(True); after=run(False)

def outcome(item,r):
    # CORRECT only if the L1 card matches the expected card. Anything else answered (a wrong L1 card OR
    # an L0 ambient/action mis-answer like "Batteria: {value}." on a knowledge question) is WRONG.
    # Empty/none is NONE. Per "mai sbagliare": NONE (honest) > WRONG (confident mistake).
    if r['tier'] in ('none','?') or r['rep'] in ('(vuoto)',''): return 'NONE'
    if r['tier']=='L1/fact' and matches(item['expect'], r2c.get(nk(r['rep']))): return 'CORRECT'
    return 'WRONG'

print("=== IN-SCOPE changes (before -> after) ===  [regress = CORRECT lost]")
reg=imp=same=0
for it in ins:
    q=it['q']; ob=outcome(it,before.get(q,{'tier':'?','rep':''})); oa=outcome(it,after.get(q,{'tier':'?','rep':''}))
    if ob==oa: same+=1; continue
    regress = (ob=='CORRECT' and oa!='CORRECT')
    improve = (oa=='CORRECT' and ob!='CORRECT') or (ob=='WRONG' and oa=='NONE')
    tag = 'REGRESS' if regress else ('IMPROVE' if improve else 'CHANGE')
    if regress: reg+=1
    elif improve: imp+=1
    rb=before.get(q,{}).get('rep','')[:24]; ra=after.get(q,{}).get('rep','')[:24]
    print(f"  [{tag:7}] exp={str(it['expect'])[:18]:<18} | {ob:<7}({rb:<24}) -> {oa:<7}({ra:<24}) | {q!r}")
print(f"\n  in-scope: same={same} improve={imp} regress={reg}  (regress = a CORRECT answer was lost)")
fb=sum(1 for x in junk if (lambda r: r['tier'] not in('none','?') and r['rep'] not in('(vuoto)',''))(before.get(x['q'],{'tier':'?','rep':''})))
fa=sum(1 for x in junk if (lambda r: r['tier'] not in('none','?') and r['rep'] not in('(vuoto)',''))(after.get(x['q'],{'tier':'?','rep':''})))
print(f"  junk FP: before={fb}  after={fa}  (of {len(junk)})")
