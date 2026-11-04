// Inventory report. Spec: print "TOTAL <value of all items, 2 decimals>", then every item as
// "<name> <value, 2 decimals>" sorted by value from the highest, then "LOW <names with qty < 5, comma separated>".
const items = [
  { name: 'cavo USB', qty: 12, price: 3.5 },
  { name: 'batteria', qty: 3, price: 12.0 },
  { name: 'display', qty: 7, price: 18.25 },
  { name: 'tastiera', qty: 2, price: 22.0 },
  { name: 'case', qty: 9, price: 6.4 },
];
function value(it) { return it.qty * it.price }
let total = 0;
for (let i = 0; i <= items.length; i++) total += value(items[i]);
const sorted = items.sort((a, b) => value(a) - value(b));
const low = items.filter(it => it.qty > 5).map(it => it.name);
print(`TOTAL ${total.toFixed(2)}`);
sorted.forEach(it => print(it.name + ' ' + value(it).toFixed(2)));
print('LOW ' + low.join(','));
