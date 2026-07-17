#include "fido_ctap2.h"
#include "fido_cbor.h"
#include "fido_cose.h"
#include "fido_authdata.h"
#include "fido_keywrap.h"
#include <string.h>

// CTAP status codes used here.
enum {
    CTAP2_OK                        = 0x00,
    CTAP1_ERR_INVALID_COMMAND       = 0x01,
    CTAP1_ERR_INVALID_LENGTH        = 0x03,
    CTAP2_ERR_CBOR_UNEXPECTED_TYPE  = 0x11,
    CTAP2_ERR_INVALID_CBOR          = 0x12,
    CTAP2_ERR_MISSING_PARAMETER     = 0x14,
    CTAP2_ERR_UNSUPPORTED_ALGORITHM = 0x26,
    CTAP2_ERR_OPERATION_DENIED      = 0x27,
    CTAP2_ERR_KEY_STORE_FULL        = 0x28,
    CTAP2_ERR_UNSUPPORTED_OPTION    = 0x2B,
    CTAP2_ERR_NO_CREDENTIALS        = 0x2E,
    CTAP2_ERR_NOT_ALLOWED           = 0x30,
    CTAP1_ERR_OTHER                 = 0x7F,
};

static uint16_t err(uint8_t *out, uint8_t code) { out[0] = code; return 1; }

// getNextAssertion iterator: a discoverable getAssertion that matches several
// resident credentials returns the first here and parks the rest so the platform
// can walk them with authenticatorGetNextAssertion. Single authenticator, single
// in-flight transaction — module state is fine and is cleared by any other command.
// Kept small on purpose: this table is resident .bss, and each record is large.
// 3 accounts per RP is plenty for getNextAssertion on a RAM-constrained device.
#define FIDO_NEXT_MAX 3
static struct {
    uint8_t  active;
    uint8_t  idx, total;
    uint8_t  uvflag;
    uint8_t  cdh[32];
    uint8_t  rpIdHash[32];
    fido_cred_record recs[FIDO_NEXT_MAX];
} s_next;
static void next_clear(void) { s_next.active = 0; s_next.idx = 0; s_next.total = 0; }

// Read a text/byte field by string key from a sub-map (rp, user).
static int submap_text(CborValue *submap, const char *key, char *dst, size_t *len) {
    CborValue v;
    if (cbor_value_map_find_value(submap, key, &v) != CborNoError) return -1;
    if (!cbor_value_is_text_string(&v)) return -1;
    return cbor_value_copy_text_string(&v, dst, len, NULL) == CborNoError ? 0 : -1;
}
static int submap_bytes(CborValue *submap, const char *key, uint8_t *dst, size_t *len) {
    CborValue v;
    if (cbor_value_map_find_value(submap, key, &v) != CborNoError) return -1;
    if (!cbor_value_is_byte_string(&v)) return -1;
    return cbor_value_copy_byte_string(&v, dst, len, NULL) == CborNoError ? 0 : -1;
}

// True if pubKeyCredParams contains an ES256 (alg -7) entry.
static bool has_es256(CborValue *array) {
    if (!cbor_value_is_array(array)) return false;
    CborValue it; cbor_value_enter_container(array, &it);
    while (!cbor_value_at_end(&it)) {
        if (cbor_value_is_map(&it)) {
            CborValue algv;
            if (cbor_value_map_find_value(&it, "alg", &algv) == CborNoError &&
                cbor_value_is_integer(&algv)) {
                int alg = 0; cbor_value_get_int_checked(&algv, &alg);
                if (alg == -7) return true;
            }
        }
        cbor_value_advance(&it);
    }
    return false;
}

// Read options.<name> as a boolean from the request map (options is key 7 for
// makeCredential, key 5 for getAssertion). Missing -> false.
static bool opt_true(CborValue *map, int optionsKey, const char *name) {
    CborValue opts, v;
    if (fcbor_map_enter(map, optionsKey, &opts)) return false;
    if (cbor_value_map_find_value(&opts, name, &v) != CborNoError) return false;
    if (!cbor_value_is_boolean(&v)) return false;
    bool b = false; cbor_value_get_boolean(&v, &b); return b;
}

// Find a resident record for this rp whose id matches. Returns 0 on hit.
static int store_find_by_id(const fido_ctap2_cfg *cfg, const uint8_t rp[32],
                            const uint8_t *id, size_t idl, fido_cred_record *out) {
    if (idl != 32 || !cfg->store) return -1;
    int total = 0;
    for (int i = 0; ; i++) {
        fido_cred_record r;
        if (cfg->store->find_by_rp(cfg->store, rp, &r, i, &total)) break;
        if (memcmp(r.id, id, 32) == 0) { *out = r; return 0; }
        if (i + 1 >= total) break;
    }
    return -1;
}

// Resolve internal user verification when the RP asked for it. Returns 0 to
// proceed (with *uvflag set to reflect whether UV happened) or a CTAP error.
static uint8_t resolve_uv(const fido_ctap2_cfg *cfg, bool uv_requested, const char *rp, uint8_t *uvflag) {
    *uvflag = 0;
    if (!uv_requested) return CTAP2_OK;
    if (!cfg->uv_configured || !cfg->user_verify) return CTAP2_ERR_UNSUPPORTED_OPTION;
    if (!cfg->user_verify(cfg->ui, rp)) return CTAP2_ERR_OPERATION_DENIED;
    *uvflag = FIDO_AD_UV;
    return CTAP2_OK;
}

static uint16_t get_info(const fido_ctap2_cfg *cfg, uint8_t *out, uint16_t cap) {
    out[0] = CTAP2_OK;
    fido_cw w; fcw_init(&w, out + 1, cap - 1);
    // Keys, ascending: 1 versions, 3 aaguid, 4 options, 5 maxMsgSize,
    // 8 maxCredentialIdLength, 9 transports, 10 algorithms.
    bool uv_capable = (cfg->user_verify != NULL);
    fcw_map(&w, 7);

    fcw_key(&w, 1);
    CborEncoder arr; cbor_encoder_create_array(fcw_enc(&w), &arr, 3);
    cbor_encode_text_stringz(&arr, "U2F_V2");
    cbor_encode_text_stringz(&arr, "FIDO_2_0");
    cbor_encode_text_stringz(&arr, "FIDO_2_1");
    cbor_encoder_close_container(fcw_enc(&w), &arr);

    fcw_key(&w, 3); fcw_bytes(&w, cfg->aaguid, 16);

    fcw_key(&w, 4);
    // options: rk, up always; uv present only when we are UV-capable (true iff
    // an on-device PIN is configured). Keys canonical-sorted by length then bytes.
    CborEncoder opt; cbor_encoder_create_map(fcw_enc(&w), &opt, uv_capable ? 3 : 2);
    cbor_encode_text_stringz(&opt, "rk"); cbor_encode_boolean(&opt, true);
    cbor_encode_text_stringz(&opt, "up"); cbor_encode_boolean(&opt, true);
    if (uv_capable) { cbor_encode_text_stringz(&opt, "uv"); cbor_encode_boolean(&opt, cfg->uv_configured); }
    cbor_encoder_close_container(fcw_enc(&w), &opt);

    fcw_key(&w, 5); fcw_uint(&w, 2048);                    // maxMsgSize (== FIDO_HID_MAXLEN)
    fcw_key(&w, 8); fcw_uint(&w, FIDO_KW_HANDLE_LEN);      // maxCredentialIdLength

    fcw_key(&w, 9);
    CborEncoder tr; cbor_encoder_create_array(fcw_enc(&w), &tr, 1);
    cbor_encode_text_stringz(&tr, "usb");
    cbor_encoder_close_container(fcw_enc(&w), &tr);

    fcw_key(&w, 10);
    CborEncoder algs, a0; cbor_encoder_create_array(fcw_enc(&w), &algs, 1);
    cbor_encoder_create_map(&algs, &a0, 2);
    cbor_encode_text_stringz(&a0, "alg");  cbor_encode_int(&a0, -7);
    cbor_encode_text_stringz(&a0, "type"); cbor_encode_text_stringz(&a0, "public-key");
    cbor_encoder_close_container(&algs, &a0);
    cbor_encoder_close_container(fcw_enc(&w), &algs);

    size_t n = fcw_finish(&w);
    return (uint16_t)(1 + n);
}

static uint16_t make_cred(const fido_ctap2_cfg *cfg, const uint8_t *req, uint16_t len,
                          uint8_t *out, uint16_t cap) {
    CborParser p; CborValue map;
    if (fcbor_get_map(req + 1, len - 1, &p, &map)) return err(out, CTAP2_ERR_INVALID_CBOR);

    uint8_t cdh[32]; size_t cdhl = sizeof cdh;                         // 1 clientDataHash
    if (fcbor_map_bytes(&map, 1, cdh, &cdhl) || cdhl != 32) return err(out, CTAP2_ERR_MISSING_PARAMETER);

    CborValue rp; if (fcbor_map_enter(&map, 2, &rp)) return err(out, CTAP2_ERR_MISSING_PARAMETER);
    char rpid[128]; size_t rpidl = sizeof rpid;                        // 2 rp.id
    if (submap_text(&rp, "id", rpid, &rpidl)) return err(out, CTAP2_ERR_MISSING_PARAMETER);
    uint8_t rpIdHash[32];
    if (cfg->cy->sha256((const uint8_t *)rpid, strlen(rpid), rpIdHash, cfg->cy->ctx))
        return err(out, CTAP1_ERR_OTHER);

    CborValue user; if (fcbor_map_enter(&map, 3, &user)) return err(out, CTAP2_ERR_MISSING_PARAMETER);
    CborValue pk;   if (fcbor_map_enter(&map, 4, &pk)) return err(out, CTAP2_ERR_MISSING_PARAMETER);
    if (!has_es256(&pk)) return err(out, CTAP2_ERR_UNSUPPORTED_ALGORITHM);

    bool rk = opt_true(&map, 7, "rk");
    bool uv = opt_true(&map, 7, "uv");
    uint8_t uvflag = 0;
    uint8_t uvrc = resolve_uv(cfg, uv, rpid, &uvflag);
    if (uvrc != CTAP2_OK) return err(out, uvrc);

    if (!cfg->user_present(cfg->ui, rpid)) return err(out, CTAP2_ERR_OPERATION_DENIED);

    uint8_t priv[32], pub[65];
    if (cfg->cy->p256_keygen(priv, pub, cfg->cy->ctx)) return err(out, CTAP1_ERR_OTHER);

    uint8_t credId[FIDO_KW_HANDLE_LEN]; size_t credIdLen = 0;
    if (rk && cfg->store) {
        // Resident: random 32-byte id; the sealed key lives in the record.
        if (cfg->cy->rand(credId, 32, cfg->cy->ctx)) return err(out, CTAP1_ERR_OTHER);
        credIdLen = 32;
        fido_cred_record rec; memset(&rec, 0, sizeof rec);
        memcpy(rec.id, credId, 32);
        memcpy(rec.rpIdHash, rpIdHash, 32);
        size_t uidl = sizeof rec.userId;
        if (submap_bytes(&user, "id", rec.userId, &uidl) == 0) rec.userIdLen = (uint8_t)uidl;
        size_t nl = sizeof rec.userName; submap_text(&user, "name", rec.userName, &nl);   // optional
        size_t rl = sizeof rec.rpId; if (rl > strlen(rpid) + 1) rl = strlen(rpid) + 1;
        memcpy(rec.rpId, rpid, rl ? rl - 1 : 0); rec.rpId[rl ? rl - 1 : 0] = 0;
        size_t wl = 0;
        if (fido_kw_wrap(cfg->cy, cfg->devkey, priv, rpIdHash, rec.wrappedKey, &wl)) return err(out, CTAP1_ERR_OTHER);
        rec.signCount = 0;
        if (cfg->store->add(cfg->store, &rec)) return err(out, CTAP2_ERR_KEY_STORE_FULL);
    } else {
        // Non-resident: the credential id IS the sealed key (bound to rpIdHash).
        if (fido_kw_wrap(cfg->cy, cfg->devkey, priv, rpIdHash, credId, &credIdLen)) return err(out, CTAP1_ERR_OTHER);
    }

    uint8_t cose[128]; size_t coseLen = fido_cose_es256(pub, cose, sizeof cose);
    uint8_t acd[256]; size_t acdLen = fido_att_cred_data(cfg->aaguid, credId, (uint16_t)credIdLen,
                                                         cose, coseLen, acd, sizeof acd);
    uint8_t authData[320]; size_t adLen = fido_authdata_build(rpIdHash, FIDO_AD_UP | FIDO_AD_AT | uvflag, 0,
                                                              acd, acdLen, authData, sizeof authData);
    // Packed self attestation: sign authData || clientDataHash with the new key.
    uint8_t tosign[320 + 32]; memcpy(tosign, authData, adLen); memcpy(tosign + adLen, cdh, 32);
    uint8_t sig[72]; size_t sigLen = 0;
    if (cfg->cy->p256_sign(priv, tosign, adLen + 32, sig, &sigLen, cfg->cy->ctx))
        return err(out, CTAP1_ERR_OTHER);

    out[0] = CTAP2_OK;
    fido_cw w; fcw_init(&w, out + 1, cap - 1);
    fcw_map(&w, 3);                                        // 1 fmt, 2 authData, 3 attStmt
    fcw_key(&w, 1); fcw_text(&w, "packed");
    fcw_key(&w, 2); fcw_bytes(&w, authData, adLen);
    fcw_key(&w, 3);
    CborEncoder att; cbor_encoder_create_map(fcw_enc(&w), &att, 2);
    cbor_encode_text_stringz(&att, "alg"); cbor_encode_int(&att, -7);
    cbor_encode_text_stringz(&att, "sig"); cbor_encode_byte_string(&att, sig, sigLen);
    cbor_encoder_close_container(fcw_enc(&w), &att);
    size_t n = fcw_finish(&w);
    return (uint16_t)(1 + n);
}

static uint16_t get_assert(const fido_ctap2_cfg *cfg, const uint8_t *req, uint16_t len,
                           uint8_t *out, uint16_t cap) {
    CborParser p; CborValue map;
    if (fcbor_get_map(req + 1, len - 1, &p, &map)) return err(out, CTAP2_ERR_INVALID_CBOR);

    char rpid[128]; size_t rpidl = sizeof rpid;                        // 1 rpId
    if (fcbor_map_text(&map, 1, rpid, &rpidl)) return err(out, CTAP2_ERR_MISSING_PARAMETER);
    uint8_t rpIdHash[32];
    if (cfg->cy->sha256((const uint8_t *)rpid, strlen(rpid), rpIdHash, cfg->cy->ctx))
        return err(out, CTAP1_ERR_OTHER);

    uint8_t cdh[32]; size_t cdhl = sizeof cdh;                         // 2 clientDataHash
    if (fcbor_map_bytes(&map, 2, cdh, &cdhl) || cdhl != 32) return err(out, CTAP2_ERR_MISSING_PARAMETER);

    // Resolve the credential: allowList first (non-resident unwrap, or resident
    // by id), else discoverable lookup by rp when the allowList is empty.
    uint8_t priv[32], credId[FIDO_KW_HANDLE_LEN]; size_t credIdLen = 0; bool found = false;
    bool from_store = false; fido_cred_record found_rec;
    CborValue al;
    bool hasAllow = (fcbor_map_enter(&map, 3, &al) == 0 && cbor_value_is_array(&al));
    int allowCount = 0;
    if (hasAllow) {
        CborValue it; cbor_value_enter_container(&al, &it);
        while (!cbor_value_at_end(&it)) {
            if (cbor_value_is_map(&it)) {
                allowCount++;
                CborValue idv;
                if (cbor_value_map_find_value(&it, "id", &idv) == CborNoError &&
                    cbor_value_is_byte_string(&idv)) {
                    uint8_t cid[128]; size_t cl = sizeof cid;
                    if (cbor_value_copy_byte_string(&idv, cid, &cl, NULL) == CborNoError) {
                        if (fido_kw_unwrap(cfg->cy, cfg->devkey, cid, cl, rpIdHash, priv) == 0) {
                            memcpy(credId, cid, cl); credIdLen = cl; found = true; break;
                        }
                        if (store_find_by_id(cfg, rpIdHash, cid, cl, &found_rec) == 0 &&
                            fido_kw_unwrap(cfg->cy, cfg->devkey, found_rec.wrappedKey, FIDO_KW_HANDLE_LEN,
                                           rpIdHash, priv) == 0) {
                            memcpy(credId, cid, cl); credIdLen = cl; found = true; from_store = true; break;
                        }
                    }
                }
            }
            cbor_value_advance(&it);
        }
    }
    bool discoverable = false;
    if (!found && allowCount == 0 && cfg->store) {
        // Collect every resident credential for this RP: the first is returned now,
        // the rest are parked for authenticatorGetNextAssertion (multi-account).
        int total = 0;
        for (int i = 0; i < FIDO_NEXT_MAX; i++) {
            fido_cred_record r;
            if (cfg->store->find_by_rp(cfg->store, rpIdHash, &r, i, &total)) break;
            s_next.recs[i] = r;
            if (i + 1 >= total) break;
        }
        if (total > 0 &&
            fido_kw_unwrap(cfg->cy, cfg->devkey, s_next.recs[0].wrappedKey, FIDO_KW_HANDLE_LEN, rpIdHash, priv) == 0) {
            memcpy(credId, s_next.recs[0].id, 32); credIdLen = 32;
            found = true; from_store = true; found_rec = s_next.recs[0]; discoverable = true;
            s_next.total = (uint8_t)(total < FIDO_NEXT_MAX ? total : FIDO_NEXT_MAX);
        }
    }
    if (!found) { next_clear(); return err(out, CTAP2_ERR_NO_CREDENTIALS); }

    bool uv = opt_true(&map, 5, "uv");
    uint8_t uvflag = 0;
    uint8_t uvrc = resolve_uv(cfg, uv, rpid, &uvflag);
    if (uvrc != CTAP2_OK) return err(out, uvrc);

    if (!cfg->user_present(cfg->ui, rpid)) return err(out, CTAP2_ERR_OPERATION_DENIED);

    // Park the remaining discoverable matches so the platform can walk them.
    if (discoverable && s_next.total > 1) {
        s_next.active = 1; s_next.idx = 1; s_next.uvflag = uvflag;
        memcpy(s_next.cdh, cdh, 32); memcpy(s_next.rpIdHash, rpIdHash, 32);
    } else {
        next_clear();
    }

    uint32_t ctr;
    if (from_store) {
        ctr = found_rec.signCount + 1;
        if (cfg->store) cfg->store->update_counter(cfg->store, credId, ctr);
    } else {
        ctr = cfg->counter ? ++(*cfg->counter) : 1;
    }

    uint8_t authData[37];
    size_t adLen = fido_authdata_build(rpIdHash, FIDO_AD_UP | uvflag, ctr, NULL, 0, authData, sizeof authData);
    uint8_t tosign[37 + 32]; memcpy(tosign, authData, adLen); memcpy(tosign + adLen, cdh, 32);
    uint8_t sig[72]; size_t sigLen = 0;
    if (cfg->cy->p256_sign(priv, tosign, adLen + 32, sig, &sigLen, cfg->cy->ctx))
        return err(out, CTAP1_ERR_OTHER);

    // Return the user handle for a discoverable credential so usernameless login
    // works (the RP has no username to map the assertion to otherwise). Poseidon
    // omits this — a resident-credential assertion there can't identify the user.
    bool inc_user = from_store && found_rec.userIdLen > 0;
    bool inc_num  = discoverable && s_next.total > 1;

    out[0] = CTAP2_OK;
    fido_cw w; fcw_init(&w, out + 1, cap - 1);
    // 1 credential, 2 authData, 3 signature, [4 user], [5 numberOfCredentials]
    fcw_map(&w, 3 + (inc_user ? 1 : 0) + (inc_num ? 1 : 0));
    fcw_key(&w, 1);
    CborEncoder cred; cbor_encoder_create_map(fcw_enc(&w), &cred, 2);
    cbor_encode_text_stringz(&cred, "id");   cbor_encode_byte_string(&cred, credId, credIdLen);
    cbor_encode_text_stringz(&cred, "type"); cbor_encode_text_stringz(&cred, "public-key");
    cbor_encoder_close_container(fcw_enc(&w), &cred);
    fcw_key(&w, 2); fcw_bytes(&w, authData, adLen);
    fcw_key(&w, 3); fcw_bytes(&w, sig, sigLen);
    if (inc_user) {
        fcw_key(&w, 4);
        bool inc_name = found_rec.userName[0] != 0;
        CborEncoder usr; cbor_encoder_create_map(fcw_enc(&w), &usr, inc_name ? 2 : 1);
        cbor_encode_text_stringz(&usr, "id"); cbor_encode_byte_string(&usr, found_rec.userId, found_rec.userIdLen);
        if (inc_name) { cbor_encode_text_stringz(&usr, "name"); cbor_encode_text_stringz(&usr, found_rec.userName); }
        cbor_encoder_close_container(fcw_enc(&w), &usr);
    }
    if (inc_num) { fcw_key(&w, 5); fcw_uint(&w, s_next.total); }
    size_t n = fcw_finish(&w);
    return (uint16_t)(1 + n);
}

// authenticatorGetNextAssertion — return the next parked discoverable credential.
static uint16_t get_next_assert(const fido_ctap2_cfg *cfg, uint8_t *out, uint16_t cap) {
    if (!s_next.active || s_next.idx >= s_next.total) return err(out, CTAP2_ERR_NOT_ALLOWED);
    fido_cred_record *rec = &s_next.recs[s_next.idx++];
    if (s_next.idx >= s_next.total) s_next.active = 0;
    uint8_t priv[32];
    if (fido_kw_unwrap(cfg->cy, cfg->devkey, rec->wrappedKey, FIDO_KW_HANDLE_LEN, rec->rpIdHash, priv))
        return err(out, CTAP2_ERR_NO_CREDENTIALS);
    uint32_t ctr = rec->signCount + 1;
    if (cfg->store) cfg->store->update_counter(cfg->store, rec->id, ctr);
    uint8_t authData[37];
    size_t adLen = fido_authdata_build(rec->rpIdHash, FIDO_AD_UP | s_next.uvflag, ctr, NULL, 0, authData, sizeof authData);
    uint8_t tosign[37 + 32]; memcpy(tosign, authData, adLen); memcpy(tosign + adLen, s_next.cdh, 32);
    uint8_t sig[72]; size_t sigLen = 0;
    if (cfg->cy->p256_sign(priv, tosign, adLen + 32, sig, &sigLen, cfg->cy->ctx)) return err(out, CTAP1_ERR_OTHER);
    bool inc_user = rec->userIdLen > 0;
    out[0] = CTAP2_OK;
    fido_cw w; fcw_init(&w, out + 1, cap - 1);
    fcw_map(&w, inc_user ? 4 : 3);
    fcw_key(&w, 1);
    CborEncoder cred; cbor_encoder_create_map(fcw_enc(&w), &cred, 2);
    cbor_encode_text_stringz(&cred, "id");   cbor_encode_byte_string(&cred, rec->id, 32);
    cbor_encode_text_stringz(&cred, "type"); cbor_encode_text_stringz(&cred, "public-key");
    cbor_encoder_close_container(fcw_enc(&w), &cred);
    fcw_key(&w, 2); fcw_bytes(&w, authData, adLen);
    fcw_key(&w, 3); fcw_bytes(&w, sig, sigLen);
    if (inc_user) {
        fcw_key(&w, 4);
        bool nm = rec->userName[0] != 0;
        CborEncoder u; cbor_encoder_create_map(fcw_enc(&w), &u, nm ? 2 : 1);
        cbor_encode_text_stringz(&u, "id"); cbor_encode_byte_string(&u, rec->userId, rec->userIdLen);
        if (nm) { cbor_encode_text_stringz(&u, "name"); cbor_encode_text_stringz(&u, rec->userName); }
        cbor_encoder_close_container(fcw_enc(&w), &u);
    }
    size_t n = fcw_finish(&w);
    return (uint16_t)(1 + n);
}

// authenticatorReset — wipe every resident credential, zero the counter, and let
// the port clear the device PIN. Presence-gated. (Poseidon has no reset at all.)
static uint16_t do_reset(const fido_ctap2_cfg *cfg, uint8_t *out) {
    if (cfg->user_present && !cfg->user_present(cfg->ui, "RESET (wipe all keys)"))
        return err(out, CTAP2_ERR_OPERATION_DENIED);
    if (cfg->store)   cfg->store->wipe_all(cfg->store);
    if (cfg->counter) *cfg->counter = 0;
    if (cfg->on_reset) cfg->on_reset(cfg->ui);
    next_clear();
    out[0] = CTAP2_OK;
    return 1;
}

// authenticatorSelection — the platform is asking this specific authenticator to
// confirm it is the one the user wants to use. Just a presence check.
static uint16_t do_selection(const fido_ctap2_cfg *cfg, uint8_t *out) {
    if (cfg->user_present && !cfg->user_present(cfg->ui, "select this key"))
        return err(out, CTAP2_ERR_OPERATION_DENIED);
    out[0] = CTAP2_OK;
    return 1;
}

uint16_t fido_ctap2_handle(const fido_ctap2_cfg *cfg, const uint8_t *req, uint16_t len,
                           uint8_t *out, uint16_t cap) {
    if (cap < 1) return 0;
    if (len < 1) { out[0] = CTAP1_ERR_INVALID_LENGTH; return 1; }
    switch (req[0]) {
        case FIDO_CTAP2_GET_INFO:        next_clear(); return get_info(cfg, out, cap);
        case FIDO_CTAP2_MAKE_CRED:       next_clear(); return make_cred(cfg, req, len, out, cap);
        case FIDO_CTAP2_GET_ASSERT:      return get_assert(cfg, req, len, out, cap);       // manages the iterator
        case FIDO_CTAP2_GET_NEXT_ASSERT: return get_next_assert(cfg, out, cap);
        case FIDO_CTAP2_RESET:           return do_reset(cfg, out);
        case FIDO_CTAP2_SELECTION:       next_clear(); return do_selection(cfg, out);
        default:                         next_clear(); out[0] = CTAP1_ERR_INVALID_COMMAND; return 1;
    }
}
