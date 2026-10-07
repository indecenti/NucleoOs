// A node --test reporter that turns each failed test into a GitHub Actions error annotation, so a red
// CI run says WHICH test failed and why right on the run page (job logs need a sign-in; annotations
// do not). Used next to the spec reporter: see .github/workflows/ci.yml.
const esc = (s) => String(s).replace(/%/g, '%25').replace(/\r/g, '').replace(/\n/g, '%0A');

export default async function* ghReporter(source) {
  for await (const ev of source) {
    if (ev.type !== 'test:fail') continue;
    const d = ev.data || {};
    const err = d.details && d.details.error;
    const cause = err && (err.cause || err);
    const msg = cause ? (cause.message || String(cause)) : 'failed';
    const file = d.file ? String(d.file).replace(/\\/g, '/').replace(/^.*?(tools\/)/, '$1') : '';
    // A parent test only repeats its failing subtest — annotate the leaves.
    if (err && err.failureType === 'subtestsFailed') continue;
    yield `::error ${file ? 'file=' + file + ',' : ''}title=${esc(d.name).slice(0, 200)}::${esc(msg).slice(0, 1500)}\n`;
  }
}
