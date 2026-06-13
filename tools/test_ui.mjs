import http from 'http';
import fs from 'fs';
import path from 'path';
import { exec } from 'child_process';

const PORT = 8080;
const ROOT = path.resolve('./');
let listApiCalls = 0;

const server = http.createServer((req, res) => {
    // 1. Endpoint per i risultati del test
    if (req.method === 'POST' && req.url === '/report') {
        let body = '';
        req.on('data', chunk => body += chunk);
        req.on('end', () => {
            const results = JSON.parse(body);
            console.log('\n--- FILE COMMANDER PRO TEST RESULTS ---');
            let passed = 0, failed = 0;
            results.forEach(r => {
                if (r.ok) { 
                    console.log(`[PASS] ${r.name}`); 
                    passed++; 
                } else { 
                    console.log(`[FAIL] ${r.name}\n       Error: ${r.error}`); 
                    failed++; 
                }
            });
            console.log(`\nTotal: ${passed + failed} | Passed: ${passed} | Failed: ${failed}`);
            res.end('ok');
            process.exit(failed > 0 ? 1 : 0);
        });
        return;
    }

    // 2. Pagina di test HTML che inietta l'app nell'iframe
    if (req.url === '/') {
        res.writeHead(200, { 'Content-Type': 'text/html' });
        res.end(`
<!DOCTYPE html>
<html>
<head><style>body{font-family:sans-serif; background:#1e1e1e; color:#fff;} iframe{border:1px solid #444; border-radius:8px;}</style></head>
<body>
    <h2>🚀 Esecuzione Test Automatici su File Commander...</h2>
    <iframe id="app" src="/apps/file-commander/www/index.html?path=/data" style="width:800px;height:500px;"></iframe>
    <script>
        const app = document.getElementById('app');
        const results = [];
        const wait = ms => new Promise(r => setTimeout(r, ms));
        
        const test = async (name, fn) => {
            try { await fn(); results.push({ name, ok: true }); }
            catch (e) { results.push({ name, ok: false, error: e.toString() }); }
        };

        app.onload = async () => {
            const win = app.contentWindow;
            const doc = app.contentDocument;
            
            // Attendi il primo load() al caricamento della pagina
            await wait(600);

            // Test 1: Elementi Base UI Pro
            await test("Pro UI Elements (Header & Toggle) exist", async () => {
                if (!doc.getElementById('list-header')) throw new Error("Header Colonne mancante");
                if (!doc.getElementById('btn-view')) throw new Error("Pulsante Toggle View mancante");
            });

            // Test 2: Toggle Grid View
            await test("Grid View Toggle funziona correttamente", async () => {
                const btn = doc.getElementById('btn-view');
                btn.click();
                await wait(100);
                if (!doc.body.classList.contains('is-grid')) throw new Error("Il body non ha la classe is-grid");
                
                const list = doc.getElementById('list');
                if (!list.classList.contains('view-grid')) throw new Error("La lista non ha la classe view-grid");
                
                // Torna alla lista
                btn.click();
            });

            // Test 3: Ordinamento Colonne
            await test("Sorting delle Colonne (Cartelle sempre in alto)", async () => {
                const header = doc.querySelector('.lh-name');
                header.click(); // Cambia ordinamento
                await wait(100);
                
                const names = [...doc.querySelectorAll('#list .name')].map(n => n.textContent);
                if (names.length > 0 && names[0] !== 'folder1') {
                    throw new Error("L'ordinamento ha spostato la cartella in basso! Trovato: " + names[0]);
                }
            });

            // Test 4: Debouncing degli eventi fs.changed
            await test("Debounce della Tempesta WebSocket (fs.changed)", async () => {
                let initialCount = await (await fetch('/api/test-count')).json();
                
                // Simuliamo una raffica di 50 eventi fs.changed (es. unzip di un file)
                for (let i = 0; i < 50; i++) {
                    win.postMessage({ t: 'fs.changed', d: { path: '/data/file'+i+'.txt' } }, '*');
                }
                
                await wait(200); // Siamo dentro i 400ms di debounce
                let midCount = await (await fetch('/api/test-count')).json();
                if (midCount > initialCount) throw new Error("Debounce fallito: API chiamata durante la raffica!");

                await wait(400); // Fine del timer debounce
                let finalCount = await (await fetch('/api/test-count')).json();
                if (finalCount > initialCount + 3 || finalCount === initialCount) {
                    throw new Error("L'API è stata chiamata " + (finalCount - initialCount) + " volte invece di 1-2 (per lista + albero).");
                }
            });

            // Invia risultati al server Node
            fetch('/report', { method: 'POST', body: JSON.stringify(results) });
            document.body.innerHTML += "<h3 style='color:#4cd07d'>✅ Test completati. Controlla il terminale!</h3>";
        };
    </script>
</body>
</html>
        `);
        return;
    }

    // 3. API Mock per il filesystem
    if (req.url === '/api/test-count') {
        res.writeHead(200, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify(listApiCalls));
        return;
    }
    
    if (req.url.startsWith('/api/fs/list')) {
        listApiCalls++;
        res.writeHead(200, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify({ entries: [
            {name: 'folder1', type: 'dir'}, 
            {name: 'zebra.txt', type: 'file', size: 100},
            {name: 'alpha.txt', type: 'file', size: 50}
        ] }));
        return;
    }

    // 4. Server File Statici per caricare file-commander e assets
    let filePath = path.join(ROOT, req.url.split('?')[0]);
    if (fs.existsSync(filePath) && fs.statSync(filePath).isFile()) {
        res.writeHead(200);
        fs.createReadStream(filePath).pipe(res);
    } else {
        res.writeHead(404);
        res.end('Not found');
    }
});

server.listen(PORT, () => {
    console.log(`[Test Server] Avviato su http://localhost:${PORT}/`);
    console.log(`[Test Server] Apro il browser per avviare la suite di test E2E UI...`);
    
    // Apri il browser automaticamente (su Windows usa 'start')
    exec(`start http://localhost:${PORT}/`);
});
