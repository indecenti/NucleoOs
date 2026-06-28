// PreToolUse guard (Bash|PowerShell): force a human confirmation before any command that
// ships to the physical device — flash / OTA / release / SD sync. Enforces the never-auto-deploy
// rule deterministically, even in auto/dontAsk permission modes. Uses "ask" (not "deny") so a
// genuine user-requested release just needs one approval instead of being impossible.
let s = '';
process.stdin.on('data', (d) => (s += d));
process.stdin.on('end', () => {
  let cmd = '';
  try { const j = JSON.parse(s); cmd = String((j.tool_input && j.tool_input.command) || ''); } catch { /* ignore */ }
  const danger = /flash\.ps1|ota\.ps1|release\.ps1|sd-sync\.ps1|push-ota|upload_ota|idf\.py\s+flash|esptool/i;
  if (danger.test(cmd)) {
    process.stdout.write(JSON.stringify({
      hookSpecificOutput: {
        hookEventName: 'PreToolUse',
        permissionDecision: 'ask',
        permissionDecisionReason: 'Guard: this flashes/deploys/OTAs to the physical Cardputer. The never-auto-deploy rule requires an EXPLICIT user request — confirm only if the user asked for this release.'
      }
    }));
  }
});
