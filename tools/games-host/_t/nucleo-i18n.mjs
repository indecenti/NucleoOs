const tr = (key) => key;
const I18N = { t: tr, scope: () => tr, init: async () => tr, onChange() {}, fmtNumber: (n) => String(n), fmtDate: (d) => String(d) };
export default I18N;
export { tr as t };
