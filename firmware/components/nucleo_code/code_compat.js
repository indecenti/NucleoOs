// code_compat.js: the library methods MicroQuickJS leaves out that models (and people) reach for by habit, in
// ES5. In CHUNKS: code_rt.c runs a chunk before a script only when the script's source names one of the chunk's
// words (a whole layer costs ~8 KB of the arena on the device; a script that needs none pays nothing).
// tools/code/gen-tables.mjs turns this file into gen/code_compat.h. Syntax (let, arrow functions, classes...)
// cannot be added here, only methods.
//@chunk Object.assign Object.values Object.entries Object.freeze
(function () {
  function def(o, k, f) { if (typeof o[k] !== 'function') o[k] = f; }
  def(Object, 'assign', function (t) { for (var i = 1; i < arguments.length; i++) { var s = arguments[i]; if (s != null) { var k = Object.keys(s); for (var j = 0; j < k.length; j++) t[k[j]] = s[k[j]]; } } return t; });
  def(Object, 'values', function (o) { var k = Object.keys(o), r = []; for (var i = 0; i < k.length; i++) r.push(o[k[i]]); return r; });
  def(Object, 'entries', function (o) { var k = Object.keys(o), r = []; for (var i = 0; i < k.length; i++) r.push([k[i], o[k[i]]]); return r; });
  def(Object, 'freeze', function (o) { return o; });
})();
//@chunk .find( .findIndex( .includes( .fill( Array.from Array.of
(function () {
  var A = Array.prototype;
  function def(o, k, f) { if (typeof o[k] !== 'function') o[k] = f; }
  def(A, 'find', function (f, t) { for (var i = 0; i < this.length; i++) if (f.call(t, this[i], i, this)) return this[i]; return undefined; });
  def(A, 'findIndex', function (f, t) { for (var i = 0; i < this.length; i++) if (f.call(t, this[i], i, this)) return i; return -1; });
  def(A, 'includes', function (v, from) { for (var i = from || 0; i < this.length; i++) if (this[i] === v || (v !== v && this[i] !== this[i])) return true; return false; });
  def(A, 'fill', function (v, s, e) { var n = this.length; s = s || 0; e = e === undefined ? n : e; for (var i = s < 0 ? n + s : s; i < (e < 0 ? n + e : e); i++) this[i] = v; return this; });
  def(Array, 'from', function (x, f) { var r = [], n = x.length || 0; for (var i = 0; i < n; i++) r.push(f ? f(x[i], i) : x[i]); return r; });
  def(Array, 'of', function () { var r = []; for (var i = 0; i < arguments.length; i++) r.push(arguments[i]); return r; });
})();
//@chunk .includes( .startsWith( .endsWith( .padStart( .padEnd(
(function () {
  var S = String.prototype;
  function def(o, k, f) { if (typeof o[k] !== 'function') o[k] = f; }
  def(S, 'includes', function (s, p) { return this.indexOf(s, p || 0) >= 0; });
  def(S, 'startsWith', function (s, p) { p = p || 0; return this.substring(p, p + s.length) === s; });
  def(S, 'endsWith', function (s, e) { e = e === undefined ? this.length : e; return this.substring(e - s.length, e) === s; });
  function pad(str, n, f, left) { str = String(str); f = f === undefined ? ' ' : String(f); if (str.length >= n || !f) return str; var p = ''; while (p.length < n - str.length) p += f; p = p.substring(0, n - str.length); return left ? p + str : str + p; }
  def(S, 'padStart', function (n, f) { return pad(this, n, f, true); });
  def(S, 'padEnd', function (n, f) { return pad(this, n, f, false); });
})();
//@chunk Number.isInteger Number.isFinite Math.hypot Math.cbrt
(function () {
  function def(o, k, f) { if (typeof o[k] !== 'function') o[k] = f; }
  def(Number, 'isInteger', function (v) { return typeof v === 'number' && isFinite(v) && Math.floor(v) === v; });
  def(Number, 'isFinite', function (v) { return typeof v === 'number' && isFinite(v); });
  def(Math, 'hypot', function () { var s = 0; for (var i = 0; i < arguments.length; i++) s += arguments[i] * arguments[i]; return Math.sqrt(s); });
  def(Math, 'cbrt', function (x) { return x < 0 ? -Math.pow(-x, 1 / 3) : Math.pow(x, 1 / 3); });
})();
//@chunk Map( Set(
(function () {
  // Small and linear (keys compared like ===), `size` kept as a property, keys()/values()/entries() return arrays.
  function Coll() { this._k = []; this._v = []; this.size = 0; }
  var P = Coll.prototype;
  P._i = function (k) { for (var i = 0; i < this._k.length; i++) if (this._k[i] === k || (k !== k && this._k[i] !== this._k[i])) return i; return -1; };
  P.has = function (k) { return this._i(k) >= 0; };
  P.get = function (k) { var i = this._i(k); return i < 0 ? undefined : this._v[i]; };
  P.set = function (k, v) { var i = this._i(k); if (i < 0) { this._k.push(k); this._v.push(v); this.size++; } else this._v[i] = v; return this; };
  P.add = function (k) { return this.set(k, k); };
  P['delete'] = function (k) { var i = this._i(k); if (i < 0) return false; this._k.splice(i, 1); this._v.splice(i, 1); this.size--; return true; };
  P.clear = function () { this._k = []; this._v = []; this.size = 0; };
  P.forEach = function (f, t) { for (var i = 0; i < this._k.length; i++) f.call(t, this._v[i], this._k[i], this); };
  P.keys = function () { return this._k.slice(); };
  P.values = function () { return this._v.slice(); };
  P.entries = function () { var r = []; for (var i = 0; i < this._k.length; i++) r.push([this._k[i], this._v[i]]); return r; };
  if (typeof globalThis.Map !== 'function') { globalThis.Map = function (it) { Coll.call(this); if (it) for (var i = 0; i < it.length; i++) this.set(it[i][0], it[i][1]); }; globalThis.Map.prototype = P; }
  if (typeof globalThis.Set !== 'function') { globalThis.Set = function (it) { Coll.call(this); if (it) for (var i = 0; i < it.length; i++) this.add(it[i]); }; globalThis.Set.prototype = Object.create(P); }
})();
