// twin-scope — where "<file>.gz" is a DERIVED twin that the device's webfs serves instead of "<file>"
// (nucleo_webfs.c), so a stale one shadows new code: exactly /www/shell/** and /apps/<id>/www/**,
// case-insensitive like FAT. Same rule as the firmware (firmware/components/nucleo_fsapi/fstwin.c) and
// sd_deploy.py twin_scope(); all three are held to tools/lib/twin-scope-vectors.json.
// Outside this scope a "<name>.gz" is an independent file (a user's backup.tar.gz) and is never touched.
export function twinScope(rel) {
  return /^(www\/shell|apps\/[^/]+\/www)\/./i.test(String(rel || ''));
}

// Card tools write the card directly (no firmware in between), so they apply the firmware's rule themselves:
// for every staged raw file in scope whose twin is NOT staged, a twin on the card is stale by definition.
// `stagedRels`: Set of SD-relative paths the tool ships. Returns the twin paths (SD-relative) to remove.
export function staleTwins(stagedRels, cardHas) {
  const out = [];
  for (const rel of stagedRels) {
    if (!twinScope(rel) || /\.gz$/i.test(rel) || stagedRels.has(rel + '.gz')) continue;
    if (cardHas(rel + '.gz')) out.push(rel + '.gz');
  }
  return out;
}
