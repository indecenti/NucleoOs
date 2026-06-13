// Mini PONG per NucleoOS Sandbox (No DOM, solo Console API)
// Creato per gestire correttamente la sandbox Web Worker.

const W = 60;
const H = 20;
const PADDLE = 5;
const FPS = 20;
const MS_PER_FRAME = Math.floor(1000 / FPS);
const MAX_SCORE = 5;

let bx = W / 2, by = H / 2;
let vx = 1.2, vy = 0.8;
let p1y = Math.floor((H - PADDLE) / 2);
let p2y = Math.floor((H - PADDLE) / 2);
let score1 = 0, score2 = 0;

// Semplice IA per muovere le racchette
function updatePaddle(paddleY, targetY) {
    const center = paddleY + PADDLE / 2;
    if (targetY < center - 0.5) return Math.max(0, paddleY - 1);
    if (targetY > center + 0.5) return Math.min(H - PADDLE, paddleY + 1);
    return paddleY;
}

function render() {
    let out = `  === NUCLEO PONG ===  [ ${score1} - ${score2} ]\n`;
    out += '+' + '-'.repeat(W) + '+\n';
    
    const bX = Math.round(bx);
    const bY = Math.round(by);
    
    for (let y = 0; y < H; y++) {
        let row = '|';
        for (let x = 0; x < W; x++) {
            if (x === 0 && y >= p1y && y < p1y + PADDLE) {
                row += '█';
            } else if (x === W - 1 && y >= p2y && y < p2y + PADDLE) {
                row += '█';
            } else if (x === bX && y === bY) {
                row += 'O';
            } else {
                row += ' ';
            }
        }
        row += '|\n';
        out += row;
    }
    out += '+' + '-'.repeat(W) + '+';
    
    console.clear();
    console.log(out);
}

async function runPong() {
    console.log("Inizializzazione Pong...");
    await os.sleep(1000);
    
    while (score1 < MAX_SCORE && score2 < MAX_SCORE) {
        // Aggiorna posizione palla
        bx += vx;
        by += vy;
        
        // Rimbalzo alto/basso
        if (by <= 0 || by >= H - 1) {
            vy = -vy;
            by = Math.max(0, Math.min(H - 1, by));
        }
        
        // Collisione racchette
        if (bx <= 1 && by >= p1y && by < p1y + PADDLE) {
            vx = Math.abs(vx) * 1.05; // Accelera leggermente
            bx = 1;
        } else if (bx >= W - 2 && by >= p2y && by < p2y + PADDLE) {
            vx = -Math.abs(vx) * 1.05;
            bx = W - 2;
        }
        
        // Punto per P2
        if (bx < 0) {
            score2++;
            bx = W / 2; by = H / 2;
            vx = 1.2; vy = (Math.random() > 0.5 ? 0.8 : -0.8);
        }
        // Punto per P1
        else if (bx >= W) {
            score1++;
            bx = W / 2; by = H / 2;
            vx = -1.2; vy = (Math.random() > 0.5 ? 0.8 : -0.8);
        }
        
        // L'IA segue la palla
        if (vx < 0) {
            p1y = updatePaddle(p1y, by);
        } else {
            p2y = updatePaddle(p2y, by);
        }
        
        render();
        await os.sleep(MS_PER_FRAME);
    }
    
    console.clear();
    console.log(`=== PARTITA FINITA ===`);
    console.log(`Risultato Finale: ${score1} - ${score2}`);
    if (score1 > score2) console.log("Ha vinto il Giocatore 1!");
    else console.log("Ha vinto il Giocatore 2!");
}

await runPong();
