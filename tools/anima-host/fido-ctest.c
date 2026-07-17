// Host test for the nucleo_fido FIDO2 core. Compiled + run by fido-check.mjs with
// MinGW gcc — no ESP-IDF, no USB. Proves the CTAP2 (getInfo/makeCredential/
// getAssertion), U2F (register/authenticate), key-wrapping (RP binding), the
// resident-credential store, and the CTAPHID framing BEFORE any device build.
//
// Crypto is a deterministic MOCK: real SHA-256 (so rpIdHash and CBOR are real)
// plus an authenticated-encryption stand-in that round-trips and binds the AAD,
// so the RP-binding and unwrap-rejection paths are genuinely exercised. Real
// ECDSA/GCM correctness is validated on device (mbedTLS) and via python-fido2.
#include "fido_crypto.h"
#include "fido_ctap2.h"
#include "fido_u2f.h"
#include "fido_keywrap.h"
#include "fido_authdata.h"
#include "fido_cred_store.h"
#include "fido_ctaphid.h"
#include "fido_pin.h"
#include "cbor.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

static int g_pass = 0, g_fail = 0;
#define CHECK(c, m) do { if (c) g_pass++; else { g_fail++; printf("  FAIL: %s (%s:%d)\n", m, __FILE__, __LINE__); } } while (0)

// ----------------------------------------------------------------------------
// SHA-256 (public-domain style compact implementation)
// ----------------------------------------------------------------------------
typedef struct { uint32_t s[8]; uint64_t len; uint8_t buf[64]; size_t n; } sha256_t;
static uint32_t ror(uint32_t x, int r){ return (x>>r)|(x<<(32-r)); }
static void sha256_blk(sha256_t *c, const uint8_t *p){
    static const uint32_t K[64]={
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
    uint32_t w[64];
    for(int i=0;i<16;i++) w[i]=(uint32_t)p[i*4]<<24|(uint32_t)p[i*4+1]<<16|(uint32_t)p[i*4+2]<<8|p[i*4+3];
    for(int i=16;i<64;i++){ uint32_t s0=ror(w[i-15],7)^ror(w[i-15],18)^(w[i-15]>>3);
        uint32_t s1=ror(w[i-2],17)^ror(w[i-2],19)^(w[i-2]>>10); w[i]=w[i-16]+s0+w[i-7]+s1; }
    uint32_t a=c->s[0],b=c->s[1],cc=c->s[2],d=c->s[3],e=c->s[4],f=c->s[5],g=c->s[6],h=c->s[7];
    for(int i=0;i<64;i++){ uint32_t S1=ror(e,6)^ror(e,11)^ror(e,25); uint32_t ch=(e&f)^(~e&g);
        uint32_t t1=h+S1+ch+K[i]+w[i]; uint32_t S0=ror(a,2)^ror(a,13)^ror(a,22);
        uint32_t maj=(a&b)^(a&cc)^(b&cc); uint32_t t2=S0+maj;
        h=g;g=f;f=e;e=d+t1;d=cc;cc=b;b=a;a=t1+t2; }
    c->s[0]+=a;c->s[1]+=b;c->s[2]+=cc;c->s[3]+=d;c->s[4]+=e;c->s[5]+=f;c->s[6]+=g;c->s[7]+=h;
}
static void sha256_init(sha256_t *c){ c->s[0]=0x6a09e667;c->s[1]=0xbb67ae85;c->s[2]=0x3c6ef372;
    c->s[3]=0xa54ff53a;c->s[4]=0x510e527f;c->s[5]=0x9b05688c;c->s[6]=0x1f83d9ab;c->s[7]=0x5be0cd19;
    c->len=0;c->n=0; }
static void sha256_upd(sha256_t *c, const uint8_t *p, size_t n){ c->len+=n;
    while(n){ size_t k=64-c->n; if(k>n)k=n; memcpy(c->buf+c->n,p,k); c->n+=k;p+=k;n-=k;
        if(c->n==64){ sha256_blk(c,c->buf); c->n=0; } } }
static void sha256_fin(sha256_t *c, uint8_t out[32]){ uint64_t bits=c->len*8; uint8_t pad=0x80;
    sha256_upd(c,&pad,1); uint8_t z=0; while(c->n!=56) sha256_upd(c,&z,1);
    uint8_t L[8]; for(int i=0;i<8;i++) L[i]=(uint8_t)(bits>>(56-8*i)); sha256_upd(c,L,8);
    for(int i=0;i<8;i++){ out[i*4]=(uint8_t)(c->s[i]>>24);out[i*4+1]=(uint8_t)(c->s[i]>>16);
        out[i*4+2]=(uint8_t)(c->s[i]>>8);out[i*4+3]=(uint8_t)c->s[i]; } }
static void sha256_all(const uint8_t *m, size_t n, uint8_t out[32]){ sha256_t c; sha256_init(&c);
    sha256_upd(&c,m,n); sha256_fin(&c,out); }
static void hmac256(const uint8_t *key, size_t kl, const uint8_t *m, size_t ml, uint8_t out[32]){
    uint8_t k[64]={0}, ip[64], op[64], ih[32]; if(kl>64){ sha256_all(key,kl,k); } else memcpy(k,key,kl);
    for(int i=0;i<64;i++){ ip[i]=k[i]^0x36; op[i]=k[i]^0x5c; }
    sha256_t c; sha256_init(&c); sha256_upd(&c,ip,64); sha256_upd(&c,m,ml); sha256_fin(&c,ih);
    sha256_init(&c); sha256_upd(&c,op,64); sha256_upd(&c,ih,32); sha256_fin(&c,out);
}

// ----------------------------------------------------------------------------
// Mock crypto vtable
// ----------------------------------------------------------------------------
static uint32_t s_rng = 0x1234abcdu;
static int m_rand(uint8_t *d, size_t n, void *x){ (void)x; for(size_t i=0;i<n;i++){
    s_rng^=s_rng<<13; s_rng^=s_rng>>17; s_rng^=s_rng<<5; d[i]=(uint8_t)s_rng; } return 0; }
static int m_sha(const uint8_t *m, size_t n, uint8_t o[32], void *x){ (void)x; sha256_all(m,n,o); return 0; }
static int m_hmac(const uint8_t *k, size_t kl, const uint8_t *m, size_t ml, uint8_t o[32], void *x){
    (void)x; hmac256(k,kl,m,ml,o); return 0; }
static int m_keygen(uint8_t priv[32], uint8_t pub[65], void *x){ (void)x; m_rand(priv,32,0);
    pub[0]=0x04; sha256_all(priv,32,pub+1); uint8_t t[33]; memcpy(t,priv,32); t[32]=1; sha256_all(t,33,pub+33); return 0; }
static int m_sign(const uint8_t priv[32], const uint8_t *m, size_t n, uint8_t *sig, size_t *sl, void *x){
    (void)x; uint8_t r[32], s[32]; sha256_all(m,n,r); uint8_t t[64]; sha256_all(m,n,t); memcpy(t+32,priv,32);
    sha256_all(t,64,s); // pseudo-DER: SEQ{ INT r, INT s }
    uint8_t *p=sig; *p++=0x30; *p++=0x44; *p++=0x02; *p++=0x20; memcpy(p,r,32); p+=32;
    *p++=0x02; *p++=0x20; memcpy(p,s,32); p+=32; *sl=(size_t)(p-sig); return 0; }
// AEAD stand-in: ks=SHA256(key||iv); ct=pt^ks; tag=HMAC(key, iv||aad||ct)[0:16].
static void ks32(const uint8_t key[32], const uint8_t iv[12], uint8_t ks[32]){
    uint8_t t[44]; memcpy(t,key,32); memcpy(t+32,iv,12); sha256_all(t,44,ks); }
static int m_seal(const uint8_t key[32], const uint8_t iv[12], const uint8_t *aad, size_t al,
                  const uint8_t *in, size_t len, uint8_t *out, uint8_t tag[16], void *x){ (void)x;
    uint8_t ks[32]; ks32(key,iv,ks); for(size_t i=0;i<len;i++) out[i]=in[i]^ks[i%32];
    uint8_t mac[32]; uint8_t buf[12+64+64]; size_t o=0; memcpy(buf+o,iv,12);o+=12;
    memcpy(buf+o,aad,al);o+=al; memcpy(buf+o,out,len);o+=len; hmac256(key,32,buf,o,mac); memcpy(tag,mac,16); return 0; }
static int m_open(const uint8_t key[32], const uint8_t iv[12], const uint8_t *aad, size_t al,
                  const uint8_t *in, size_t len, const uint8_t tag[16], uint8_t *out, void *x){ (void)x;
    uint8_t mac[32]; uint8_t buf[12+64+64]; size_t o=0; memcpy(buf+o,iv,12);o+=12;
    memcpy(buf+o,aad,al);o+=al; memcpy(buf+o,in,len);o+=len; hmac256(key,32,buf,o,mac);
    if(memcmp(mac,tag,16)!=0) return -1;
    uint8_t ks[32]; ks32(key,iv,ks);
    for(size_t i=0;i<len;i++) out[i]=in[i]^ks[i%32];
    return 0; }

static const fido_crypto_t CY = { m_rand, m_sha, m_hmac, m_keygen, m_sign, m_seal, m_open, NULL };

// ----------------------------------------------------------------------------
// Test fixtures
// ----------------------------------------------------------------------------
static const uint8_t AAGUID[16] = { 'N','U','C','L','E','O','O','S','F','I','D','O','2','K','E','Y' };
static uint32_t g_counter = 5;
static int s_up = 1, s_uv = 1;
static int up_cb(void *x, const char *rp){ (void)x; (void)rp; return s_up; }
static int uv_cb(void *x, const char *rp){ (void)x; (void)rp; return s_uv; }
static int u2f_up_cb(void *x){ (void)x; return s_up; }   // U2F presence has no RP name

// CBOR request builders --------------------------------------------------------
static uint16_t build_makecred(uint8_t *req, size_t cap, const char *rpid, bool rk, bool uv, uint8_t uidb){
    req[0]=FIDO_CTAP2_MAKE_CRED;
    CborEncoder e, m, sub, arr, a0; cbor_encoder_init(&e, req+1, cap-1, 0);
    cbor_encoder_create_map(&e, &m, (rk||uv)?5:4);
    uint8_t cdh[32]; memset(cdh,0xA5,32);
    cbor_encode_int(&m,1); cbor_encode_byte_string(&m,cdh,32);
    cbor_encode_int(&m,2); cbor_encoder_create_map(&m,&sub,1);
        cbor_encode_text_stringz(&sub,"id"); cbor_encode_text_stringz(&sub,rpid);
        cbor_encoder_close_container(&m,&sub);
    cbor_encode_int(&m,3); cbor_encoder_create_map(&m,&sub,2);
        uint8_t uid[16]; memset(uid,uidb,16);
        cbor_encode_text_stringz(&sub,"id"); cbor_encode_byte_string(&sub,uid,16);
        cbor_encode_text_stringz(&sub,"name"); cbor_encode_text_stringz(&sub,"alice");
        cbor_encoder_close_container(&m,&sub);
    cbor_encode_int(&m,4); cbor_encoder_create_array(&m,&arr,1);
        cbor_encoder_create_map(&arr,&a0,2);
        cbor_encode_text_stringz(&a0,"alg"); cbor_encode_int(&a0,-7);
        cbor_encode_text_stringz(&a0,"type"); cbor_encode_text_stringz(&a0,"public-key");
        cbor_encoder_close_container(&arr,&a0);
        cbor_encoder_close_container(&m,&arr);
    if(rk||uv){ cbor_encode_int(&m,7); CborEncoder o; cbor_encoder_create_map(&m,&o,(rk&&uv)?2:1);
        if(rk){ cbor_encode_text_stringz(&o,"rk"); cbor_encode_boolean(&o,true); }
        if(uv){ cbor_encode_text_stringz(&o,"uv"); cbor_encode_boolean(&o,true); }
        cbor_encoder_close_container(&m,&o); }
    cbor_encoder_close_container(&e,&m);
    return (uint16_t)(1+cbor_encoder_get_buffer_size(&e, req+1));
}
static uint16_t build_getassert(uint8_t *req, size_t cap, const char *rpid,
                                const uint8_t *allowId, size_t allowIdLen){
    req[0]=FIDO_CTAP2_GET_ASSERT;
    CborEncoder e, m, arr, d; cbor_encoder_init(&e, req+1, cap-1, 0);
    cbor_encoder_create_map(&e,&m, allowId?3:2);
    cbor_encode_int(&m,1); cbor_encode_text_stringz(&m,rpid);
    uint8_t cdh[32]; memset(cdh,0x5A,32);
    cbor_encode_int(&m,2); cbor_encode_byte_string(&m,cdh,32);
    if(allowId){ cbor_encode_int(&m,3); cbor_encoder_create_array(&m,&arr,1);
        cbor_encoder_create_map(&arr,&d,2);
        cbor_encode_text_stringz(&d,"id"); cbor_encode_byte_string(&d,allowId,allowIdLen);
        cbor_encode_text_stringz(&d,"type"); cbor_encode_text_stringz(&d,"public-key");
        cbor_encoder_close_container(&arr,&d); cbor_encoder_close_container(&m,&arr); }
    cbor_encoder_close_container(&e,&m);
    return (uint16_t)(1+cbor_encoder_get_buffer_size(&e, req+1));
}

// Extract credId from a makeCredential response authData (key 2).
static int resp_credid(const uint8_t *resp, uint16_t rl, uint8_t *idout, uint16_t *idlen){
    CborParser p; CborValue m;
    if(cbor_parser_init(resp+1, rl-1, 0, &p, &m)!=CborNoError || !cbor_value_is_map(&m)) return -1;
    CborValue it; cbor_value_enter_container(&m,&it);
    uint8_t ad[512]; size_t adl=sizeof ad; int have=0;
    while(!cbor_value_at_end(&it)){
        int k=-1; if(cbor_value_is_integer(&it)) cbor_value_get_int_checked(&it,&k);
        cbor_value_advance(&it);
        if(k==2 && cbor_value_is_byte_string(&it)){ cbor_value_copy_byte_string(&it,ad,&adl,NULL); have=1; }
        cbor_value_advance(&it);
    }
    if(!have) return -1;
    // authData: rpIdHash(32)+flags(1)+count(4)+aaguid(16)+credIdLen(2)+credId
    if(adl < 55) return -1;
    uint16_t cl = (uint16_t)ad[53]<<8 | ad[54];
    if(55u+cl > adl) return -1;
    memcpy(idout, ad+55, cl); *idlen=cl; return 0;
}
static uint8_t resp_flags(const uint8_t *resp, uint16_t rl){
    CborParser p; CborValue m, it; if(cbor_parser_init(resp+1,rl-1,0,&p,&m)) return 0;
    cbor_value_enter_container(&m,&it);
    while(!cbor_value_at_end(&it)){ int k=-1; if(cbor_value_is_integer(&it)) cbor_value_get_int_checked(&it,&k);
        cbor_value_advance(&it);
        if(k==2 && cbor_value_is_byte_string(&it)){ uint8_t ad[512]; size_t adl=sizeof ad;
            cbor_value_copy_byte_string(&it,ad,&adl,NULL); return adl>32?ad[32]:0; }
        cbor_value_advance(&it); }
    return 0;
}

// ----------------------------------------------------------------------------
// CTAPHID sink capture
// ----------------------------------------------------------------------------
static uint8_t s_hidout[4096]; static int s_hidn;
static void hid_sink(const uint8_t pkt[64], void *x){ (void)x; if(s_hidn+64<=(int)sizeof s_hidout){ memcpy(s_hidout+s_hidn,pkt,64); s_hidn+=64; } }

int main(void){
    printf("== nucleo_fido host test ==\n");
    static uint8_t out[2048], req[2048];

    fido_ctap2_cfg c2; memset(&c2,0,sizeof c2);
    c2.cy=&CY; static uint8_t devkey[32]; for(int i=0;i<32;i++) devkey[i]=(uint8_t)(i*7+1);
    c2.devkey=devkey; c2.aaguid=AAGUID; c2.counter=&g_counter;
    c2.user_present=up_cb; c2.user_verify=uv_cb; c2.uv_configured=true; c2.ui=NULL;
    c2.store=fido_cred_store_ram();
    c2.store->wipe_all(c2.store);

    // 1) getInfo -----------------------------------------------------------------
    { uint8_t g=FIDO_CTAP2_GET_INFO; uint16_t n=fido_ctap2_handle(&c2,&g,1,out,sizeof out);
      CHECK(n>1 && out[0]==0x00, "getInfo status OK");
      CborParser p; CborValue m; CHECK(cbor_parser_init(out+1,n-1,0,&p,&m)==CborNoError && cbor_value_is_map(&m), "getInfo is a map");
      // find versions (key1) contains FIDO_2_0; aaguid (key3) 16 bytes
      CborValue it; cbor_value_enter_container(&m,&it); int seen_ver=0, seen_aaguid=0, seen_uv=0;
      while(!cbor_value_at_end(&it)){ int k=-1; if(cbor_value_is_integer(&it)) cbor_value_get_int_checked(&it,&k);
        cbor_value_advance(&it);
        if(k==1 && cbor_value_is_array(&it)){ CborValue a; cbor_value_enter_container(&it,&a);
            while(!cbor_value_at_end(&a)){ char v[16]; size_t vl=sizeof v; if(cbor_value_is_text_string(&a)){
                cbor_value_copy_text_string(&a,v,&vl,NULL); if(strcmp(v,"FIDO_2_0")==0) seen_ver=1; } cbor_value_advance(&a);} }
        else if(k==3 && cbor_value_is_byte_string(&it)){ size_t bl=0; cbor_value_get_string_length(&it,&bl); seen_aaguid=(bl==16); }
        else if(k==4 && cbor_value_is_map(&it)){ CborValue uvv; if(cbor_value_map_find_value(&it,"uv",&uvv)==CborNoError && cbor_value_is_boolean(&uvv)) seen_uv=1; }
        cbor_value_advance(&it); }
      CHECK(seen_ver, "getInfo advertises FIDO_2_0");
      CHECK(seen_aaguid, "getInfo aaguid is 16 bytes");
      CHECK(seen_uv, "getInfo advertises uv option (UV-capable)");
    }

    // 2) makeCredential non-resident --------------------------------------------
    uint8_t credid[128]; uint16_t credlen=0;
    { uint16_t rl=build_makecred(req,sizeof req,"example.com",false,false,0x11);
      uint16_t n=fido_ctap2_handle(&c2,req,rl,out,sizeof out);
      CHECK(n>1 && out[0]==0x00, "makeCred(non-resident) OK");
      uint8_t fl=resp_flags(out,n); CHECK((fl&FIDO_AD_UP)&&(fl&FIDO_AD_AT), "makeCred flags UP|AT");
      CHECK(resp_credid(out,n,credid,&credlen)==0, "makeCred credId extractable");
      CHECK(credlen==FIDO_KW_HANDLE_LEN, "non-resident credId is the 60-byte wrapped key");
    }

    // 3) getAssertion with that allowList credential -----------------------------
    { uint32_t before=g_counter;
      uint16_t rl=build_getassert(req,sizeof req,"example.com",credid,credlen);
      uint16_t n=fido_ctap2_handle(&c2,req,rl,out,sizeof out);
      CHECK(n>1 && out[0]==0x00, "getAssertion(allowList) OK");
      CHECK(g_counter==before+1, "non-resident assertion bumps global counter");
    }
    // 3b) wrong RP must NOT unwrap (key-wrap RP binding) --------------------------
    { uint16_t rl=build_getassert(req,sizeof req,"evil.com",credid,credlen);
      uint16_t n=fido_ctap2_handle(&c2,req,rl,out,sizeof out);
      CHECK(n==1 && out[0]==0x2E, "handle bound to RP: wrong rpId -> NO_CREDENTIALS"); }

    // 4) makeCredential resident -------------------------------------------------
    { uint16_t rl=build_makecred(req,sizeof req,"passkey.example",true,false,0x11);
      uint16_t n=fido_ctap2_handle(&c2,req,rl,out,sizeof out);
      CHECK(n>1 && out[0]==0x00, "makeCred(resident) OK");
      uint8_t id[128]; uint16_t il=0; CHECK(resp_credid(out,n,id,&il)==0 && il==32, "resident credId is 32 random bytes");
      CHECK(c2.store->count(c2.store)==1, "resident credential stored"); }

    // 5) discoverable getAssertion (empty allowList) returns user handle ---------
    { uint16_t rl=build_getassert(req,sizeof req,"passkey.example",NULL,0);
      uint16_t n=fido_ctap2_handle(&c2,req,rl,out,sizeof out);
      CHECK(n>1 && out[0]==0x00, "discoverable getAssertion OK");
      // user (key 4) present with id
      CborParser p; CborValue m,it; cbor_parser_init(out+1,n-1,0,&p,&m); cbor_value_enter_container(&m,&it);
      int has_user=0; while(!cbor_value_at_end(&it)){ int k=-1; if(cbor_value_is_integer(&it)) cbor_value_get_int_checked(&it,&k);
        cbor_value_advance(&it); if(k==4 && cbor_value_is_map(&it)){ CborValue uidv;
            if(cbor_value_map_find_value(&it,"id",&uidv)==CborNoError && cbor_value_is_byte_string(&uidv)) has_user=1; }
        cbor_value_advance(&it); }
      CHECK(has_user, "discoverable assertion returns user handle (usernameless)"); }

    // 6) user presence denied ----------------------------------------------------
    { s_up=0; uint16_t rl=build_getassert(req,sizeof req,"example.com",credid,credlen);
      uint16_t n=fido_ctap2_handle(&c2,req,rl,out,sizeof out);
      CHECK(n==1 && out[0]==0x27, "presence denied -> OPERATION_DENIED"); s_up=1; }

    // 7) unknown command ---------------------------------------------------------
    { uint8_t bad=0x33; uint16_t n=fido_ctap2_handle(&c2,&bad,1,out,sizeof out);
      CHECK(n==1 && out[0]==0x01, "unknown CTAP2 cmd -> INVALID_COMMAND"); }

    // 7b) FIDO 2.1: FIDO_2_1 version, getNextAssertion, selection, reset ---------
    { uint8_t g=FIDO_CTAP2_GET_INFO; uint16_t n=fido_ctap2_handle(&c2,&g,1,out,sizeof out);
      int v21=0; CborParser p; CborValue m,it; cbor_parser_init(out+1,n-1,0,&p,&m); cbor_value_enter_container(&m,&it);
      while(!cbor_value_at_end(&it)){ int k=-1; if(cbor_value_is_integer(&it)) cbor_value_get_int_checked(&it,&k); cbor_value_advance(&it);
        if(k==1 && cbor_value_is_array(&it)){ CborValue a; cbor_value_enter_container(&it,&a);
          while(!cbor_value_at_end(&a)){ char v[16]; size_t vl=sizeof v; if(cbor_value_is_text_string(&a)){ cbor_value_copy_text_string(&a,v,&vl,NULL); if(strcmp(v,"FIDO_2_1")==0) v21=1; } cbor_value_advance(&a);} }
        cbor_value_advance(&it); }
      CHECK(v21, "getInfo advertises FIDO_2_1"); }

    { // two resident creds, one RP: discoverable getAssertion reports 2, getNext walks them
      uint16_t rl=build_makecred(req,sizeof req,"multi.example",true,false,0xA1); fido_ctap2_handle(&c2,req,rl,out,sizeof out);
      rl=build_makecred(req,sizeof req,"multi.example",true,false,0xB2); fido_ctap2_handle(&c2,req,rl,out,sizeof out);
      rl=build_getassert(req,sizeof req,"multi.example",NULL,0);
      uint16_t n=fido_ctap2_handle(&c2,req,rl,out,sizeof out);
      CHECK(n>1 && out[0]==0x00, "multi-account getAssertion OK");
      int num=0; CborParser p; CborValue m,it; cbor_parser_init(out+1,n-1,0,&p,&m); cbor_value_enter_container(&m,&it);
      while(!cbor_value_at_end(&it)){ int k=-1; if(cbor_value_is_integer(&it)) cbor_value_get_int_checked(&it,&k); cbor_value_advance(&it);
        if(k==5 && cbor_value_is_unsigned_integer(&it)){ uint64_t u=0; cbor_value_get_uint64(&it,&u); num=(int)u; } cbor_value_advance(&it); }
      CHECK(num==2, "getAssertion reports numberOfCredentials == 2");
      uint8_t gn=FIDO_CTAP2_GET_NEXT_ASSERT;
      uint16_t n2=fido_ctap2_handle(&c2,&gn,1,out,sizeof out);
      CHECK(n2>1 && out[0]==0x00, "getNextAssertion returns the 2nd credential");
      uint16_t n3=fido_ctap2_handle(&c2,&gn,1,out,sizeof out);
      CHECK(n3==1 && out[0]==0x30, "getNextAssertion past the end -> NOT_ALLOWED"); }

    { uint8_t sel=FIDO_CTAP2_SELECTION; uint16_t n=fido_ctap2_handle(&c2,&sel,1,out,sizeof out);
      CHECK(n==1 && out[0]==0x00, "authenticatorSelection OK (user present)"); }

    { // store global enumeration + delete (powers the on-device passkey manager)
      int before=c2.store->count(c2.store); fido_cred_record r;
      CHECK(before>0 && c2.store->get_at(c2.store,0,&r)==0, "store get_at(0) returns a resident cred");
      CHECK(c2.store->get_at(c2.store,99,&r)!=0, "store get_at out-of-range fails");
      CHECK(c2.store->remove(c2.store,r.id)==0 && c2.store->count(c2.store)==before-1, "store delete removes one cred"); }

    { uint8_t rst=FIDO_CTAP2_RESET; uint16_t n=fido_ctap2_handle(&c2,&rst,1,out,sizeof out);
      CHECK(n==1 && out[0]==0x00, "authenticatorReset OK");
      CHECK(c2.store->count(c2.store)==0, "reset wiped the resident store"); }

    // 7d) internal-UV PIN verifier (hardware-bound + lockout) -------------------
    { fido_pin_state ps; fido_pin_init(&ps);
      CHECK(!fido_pin_is_set(&ps) && fido_pin_retries(&ps)==FIDO_PIN_RETRIES, "PIN starts unset, full retries");
      CHECK(fido_pin_set(&ps,&CY,devkey,"12")==-1, "too-short PIN rejected");
      CHECK(fido_pin_set(&ps,&CY,devkey,"1379")==0 && fido_pin_is_set(&ps), "PIN set OK");
      CHECK(fido_pin_check(&ps,&CY,devkey,"1379")==1, "correct PIN verifies");
      CHECK(fido_pin_retries(&ps)==FIDO_PIN_RETRIES, "correct PIN keeps retries full");
      CHECK(fido_pin_check(&ps,&CY,devkey,"0000")==0 && fido_pin_retries(&ps)==FIDO_PIN_RETRIES-1, "wrong PIN decrements retries");
      CHECK(fido_pin_check(&ps,&CY,devkey,"1379")==1 && fido_pin_retries(&ps)==FIDO_PIN_RETRIES, "correct PIN resets retries");
      uint8_t key2[32]; for(int i=0;i<32;i++) key2[i]=(uint8_t)(i*3+9);
      CHECK(fido_pin_check(&ps,&CY,key2,"1379")==0, "PIN bound to eFuse key: a different key fails");
      fido_pin_state pl; fido_pin_init(&pl); fido_pin_set(&pl,&CY,devkey,"4242");
      int last=0; for(int i=0;i<FIDO_PIN_RETRIES;i++) last=fido_pin_check(&pl,&CY,devkey,"9999");
      CHECK(last==0 && fido_pin_retries(&pl)==0, "repeated wrong PIN exhausts retries");
      CHECK(fido_pin_check(&pl,&CY,devkey,"4242")==-1, "locked out even with the correct PIN"); }

    // 8) U2F register + authenticate --------------------------------------------
    { static const uint8_t ATT_CERT[4]={0x30,0x02,0x05,0x00}; static const uint8_t ATT_PRIV[32]={9};
      fido_u2f_cfg u; memset(&u,0,sizeof u); u.cy=&CY; u.devkey=devkey;
      u.att_cert=ATT_CERT; u.att_cert_len=4; u.att_priv=ATT_PRIV; u.counter=&g_counter;
      u.user_present=u2f_up_cb; u.ui=NULL;
      // REGISTER: CLA INS P1 P2 Lc(3) [chal32 app32]
      uint8_t apdu[7+64]; memset(apdu,0,sizeof apdu); apdu[1]=0x01; apdu[6]=64;
      memset(apdu+7,0xC1,32); memset(apdu+7+32,0xD2,32);
      uint16_t n=fido_u2f_handle(&u,apdu,sizeof apdu,out,sizeof out);
      CHECK(n>=2 && out[n-2]==0x90 && out[n-1]==0x00, "U2F register SW=9000");
      CHECK(out[0]==0x05, "U2F register reserved byte 0x05");
      uint8_t khl=out[1+65]; const uint8_t *kh=out+1+65+1;
      CHECK(khl==FIDO_KW_HANDLE_LEN, "U2F key handle is 60 bytes");
      // AUTHENTICATE: Lc = 32+32+1+khl ; [chal app khl handle]
      uint8_t a2[7+32+32+1+FIDO_KW_HANDLE_LEN]; memset(a2,0,sizeof a2);
      a2[1]=0x02; a2[2]=0x03; uint32_t lc=32+32+1+khl; a2[4]=(uint8_t)(lc>>16);a2[5]=(uint8_t)(lc>>8);a2[6]=(uint8_t)lc;
      memset(a2+7,0xC1,32); memset(a2+7+32,0xD2,32); a2[7+64]=khl; memcpy(a2+7+65,kh,khl);
      uint32_t before=g_counter; uint16_t n2=fido_u2f_handle(&u,a2,7+64+1+khl,out,sizeof out);
      CHECK(n2>=2 && out[n2-2]==0x90 && out[n2-1]==0x00, "U2F authenticate SW=9000");
      CHECK(out[0]==0x01, "U2F authenticate user-presence byte set");
      CHECK(g_counter==before+1, "U2F authenticate bumps counter");
      // Wrong appId: the handle won't unwrap under it -> SW_WRONG_DATA 6A80.
      memset(a2+7+32,0xEE,32); uint16_t n3=fido_u2f_handle(&u,a2,7+64+1+khl,out,sizeof out);
      CHECK(n3==2 && out[0]==0x6A && out[1]==0x80, "U2F wrong appId -> 6A80 (handle not ours)"); }

    // 9) CTAPHID INIT + CBOR getInfo over the transport --------------------------
    { static uint8_t rbuf[2048]; fido_ctaphid_ctx hc;
      fido_ctaphid_init(&hc, hid_sink, NULL, rbuf, sizeof rbuf);
      // Wire the CBOR handler to CTAP2 through a file-scope thunk (a plain
      // function pointer can't capture c2).
      extern uint16_t fido_test_cbor_thunk(const uint8_t*,uint16_t,uint8_t*,uint16_t,void*);
      fido_ctaphid_set_cbor(&hc, fido_test_cbor_thunk, &c2);

      // INIT on broadcast channel
      s_hidn=0; uint8_t pkt[64]; memset(pkt,0,64); pkt[0]=pkt[1]=pkt[2]=pkt[3]=0xFF;
      pkt[4]=0x86; pkt[5]=0; pkt[6]=8; for(int i=0;i<8;i++) pkt[7+i]=(uint8_t)(i+1);
      fido_ctaphid_rx(&hc,pkt);
      CHECK(s_hidn==64, "INIT produced one response packet");
      CHECK(memcmp(s_hidout+7,pkt+7,8)==0, "INIT echoes nonce");
      uint32_t newcid=(uint32_t)s_hidout[15]<<24|s_hidout[16]<<16|s_hidout[17]<<8|s_hidout[18];
      CHECK(newcid!=0 && newcid!=0xFFFFFFFF, "INIT allocated a channel id");

      // CBOR getInfo on the new channel
      s_hidn=0; memset(pkt,0,64); pkt[0]=(uint8_t)(newcid>>24);pkt[1]=(uint8_t)(newcid>>16);pkt[2]=(uint8_t)(newcid>>8);pkt[3]=(uint8_t)newcid;
      pkt[4]=0x90; pkt[5]=0; pkt[6]=1; pkt[7]=FIDO_CTAP2_GET_INFO;
      fido_ctaphid_rx(&hc,pkt);
      CHECK(s_hidn>=64 && s_hidout[4]==0x90, "CBOR response framed as CTAPHID_CBOR");
      uint16_t blen=(uint16_t)s_hidout[5]<<8|s_hidout[6];
      CHECK(blen>1 && s_hidout[7]==0x00, "framed CBOR getInfo status OK"); }

    printf("== %d passed, %d failed ==\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

// File-scope thunk so the CTAPHID CBOR callback can reach the CTAP2 handler.
uint16_t fido_test_cbor_thunk(const uint8_t *req, uint16_t rl, uint8_t *resp, uint16_t cap, void *ctx){
    return fido_ctap2_handle((const fido_ctap2_cfg*)ctx, req, rl, resp, cap);
}
