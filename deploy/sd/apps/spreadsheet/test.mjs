import assert from 'assert';

console.log('🧪 Avvio Unit Tests per Spreadsheet & ANIMA Copilot Integration');

// 1. Test della Magic Insert Regex usata in sendAnima()
function extractFormula(replyText) {
    const formulaMatch = replyText.match(/`(=[A-Z0-9(),\s<>=:\.\-\+]+)`/i);
    return formulaMatch ? formulaMatch[1] : null;
}

assert.strictEqual(
    extractFormula("Ecco la formula: `=SUM(A1:B10)`."), 
    "=SUM(A1:B10)", 
    "Errore: Regex fallita nell'estrarre SUM"
);

assert.strictEqual(
    extractFormula("Usa questa formula: `=IF(C1>10, 1, 0)`"), 
    "=IF(C1>10, 1, 0)", 
    "Errore: Regex fallita nell'estrarre IF"
);

assert.strictEqual(
    extractFormula("La media è data da `=AVG(A1:A10000)`"), 
    "=AVG(A1:A10000)", 
    "Errore: Regex fallita nell'estrarre AVG"
);

assert.strictEqual(
    extractFormula("Nessuna formula qui, solo testo."), 
    null, 
    "Errore: Regex non dovrebbe matchare testo normale"
);

console.log('✅ Test Regex completati con successo.');

// 2. Test mock della logica a_solve_spreadsheet (Simulazione output del C)
function simulateCSolver(query) {
    // Replica della logica C (a_solve_spreadsheet)
    const norm = query.toLowerCase();
    
    let fn = null;
    if (norm.includes('somma')) fn = "SUM";
    else if (norm.includes('media')) fn = "AVG";
    else if (norm.includes('se ') || norm.includes('condizione')) fn = "IF";
    else if (norm.includes('arrotonda')) fn = "ROUND";
    
    // Regex per le celle usata per simulare it[i].w
    const cellRegex = /[a-z][0-9]+/g;
    let match;
    const cells = [];
    while ((match = cellRegex.exec(norm)) !== null) {
        cells.push(match[0].toUpperCase());
    }
    
    if (!fn) return null;
    
    const isCol = norm.includes('colonna');
    const colMatch = norm.match(/colonna\s+([a-z])/);
    
    if (isCol && colMatch) {
        return `=${fn}(${colMatch[1].toUpperCase()}1:${colMatch[1].toUpperCase()}10000)`;
    }
    
    if (norm.includes('da ') && norm.includes(' a ') && cells.length >= 2) {
        return `=${fn}(${cells[0]}:${cells[1]})`;
    }
    
    if (cells.length === 1) {
        if (fn === 'IF') return `=${fn}(${cells[0]}>0, 1, 0)`;
        return `=${fn}(${cells[0]}:${cells[0][0]}10)`;
    }
    
    if (cells.length >= 2) {
        return `=${fn}(${cells[0]}:${cells[1]})`;
    }
    
    return null;
}

assert.strictEqual(simulateCSolver("Qual è la somma da a1 a b5?"), "=SUM(A1:B5)");
assert.strictEqual(simulateCSolver("Calcola la media della colonna c"), "=AVG(C1:C10000)");
assert.strictEqual(simulateCSolver("Se a1 è maggiore"), "=IF(A1>0, 1, 0)");

console.log('✅ Test simulazione logica Firmware L0 C completati con successo.');
console.log('Tutti i controlli passati!');
