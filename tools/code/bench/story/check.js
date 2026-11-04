var bad = [];
function words(s) { return s.split(/\s+/).filter(function (w) { return w.length > 0; }).length; }
for (var i = 1; i <= 3; i++) {
  var f = 'chapters/0' + i + '.md', t = fs.read(f);
  if (!t) { bad.push('missing ' + f); continue; }
  if (t.indexOf('# ') != 0) bad.push(f + ' has no # title');
  if (words(t) < 230) bad.push(f + ' has ' + words(t) + ' words');
  if (t.indexOf('Lia') < 0) bad.push(f + ' never names Lia');
}
var ix = fs.read('index.md');
if (!ix) bad.push('missing index.md'); else for (var j = 1; j <= 3; j++) if (ix.indexOf('0' + j + '.md') < 0) bad.push('index.md does not list 0' + j + '.md');
print(bad.length ? 'CHECK FAIL: ' + bad.join(' | ') : 'CHECK OK');
