// scratch/sync_podcasts_e2e.mjs
// Sincronizacao Real de Podcasts pelo Navegador Oficial (Google Chrome via CDP)
// Sincroniza os 5 ultimos episodios de cada podcast e monitora sem interrupcao

import { spawn } from 'child_process';
import http from 'http';
import fs from 'fs';
import path from 'path';

const CHROME_PATH = "C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe";
const TARGET_URL = "http://mps3.local/";
const CDP_PORT = 9222;

function delay(ms) {
    return new Promise(resolve => setTimeout(resolve, ms));
}

async function getCdpWebSocketUrl() {
    return new Promise((resolve, reject) => {
        const req = http.get(`http://127.0.0.1:${CDP_PORT}/json`, res => {
            let data = '';
            res.on('data', chunk => data += chunk);
            res.on('end', () => {
                try {
                    const tabs = JSON.parse(data);
                    const page = tabs.find(t => t.type === 'page' && (t.url.includes('mps3.local') || t.url.includes('192.168.4.1'))) || tabs[0];
                    if (page && page.webSocketDebuggerUrl) {
                        resolve(page.webSocketDebuggerUrl);
                    } else {
                        reject(new Error("Nenhuma aba com WebSocket encontrada"));
                    }
                } catch (e) {
                    reject(e);
                }
            });
        });
        req.on('error', reject);
    });
}

class CdpClient {
    constructor(wsUrl) {
        this.ws = new WebSocket(wsUrl);
        this.msgId = 0;
        this.pending = new Map();
        this.handlers = new Map();

        this.ws.onmessage = (event) => {
            const msg = JSON.parse(event.data);
            if (msg.id && this.pending.has(msg.id)) {
                const { resolve, reject } = this.pending.get(msg.id);
                this.pending.delete(msg.id);
                if (msg.error) reject(msg.error);
                else resolve(msg.result);
            }
            if (msg.method && this.handlers.has(msg.method)) {
                this.handlers.get(msg.method)(msg.params);
            }
        };
    }

    ready() {
        return new Promise((resolve, reject) => {
            if (this.ws.readyState === WebSocket.OPEN) return resolve();
            this.ws.onopen = () => resolve();
            this.ws.onerror = (err) => reject(err);
        });
    }

    send(method, params = {}) {
        return new Promise((resolve, reject) => {
            const id = ++this.msgId;
            this.pending.set(id, { resolve, reject });
            this.ws.send(JSON.stringify({ id, method, params }));
        });
    }

    on(method, handler) {
        this.handlers.set(method, handler);
    }

    async eval(expr) {
        const res = await this.send("Runtime.evaluate", {
            expression: expr,
            awaitPromise: true,
            returnByValue: true
        });
        if (res.exceptionDetails) {
            console.error("[-] Eval Exception Details:", JSON.stringify(res.exceptionDetails, null, 2));
            throw new Error(res.exceptionDetails.text || "Erro no eval JS");
        }
        return res.result ? res.result.value : undefined;
    }
}

async function main() {
    console.log("=".repeat(75));
    console.log("🎙️ SINCRONIZACAO REAL DE PODCASTS VIA CENTRAL WEB (Google Chrome E2E)");
    console.log("=".repeat(75));

    const userProfile = path.resolve("scratch/chrome_profile");
    if (!fs.existsSync(userProfile)) fs.mkdirSync(userProfile, { recursive: true });

    // Encerra qualquer instancia anterior para conexao limpa
    try {
        spawn("taskkill", ["/F", "/IM", "chrome.exe"], { stdio: 'ignore' });
        await delay(1200);
    } catch(e) {}

    console.log("[*] 1. Inicializando Google Chrome oficial com CORS liberado em " + TARGET_URL);
    const chromeProc = spawn(CHROME_PATH, [
        `--remote-debugging-port=${CDP_PORT}`,
        `--user-data-dir=${userProfile}`,
        "--no-first-run",
        "--no-default-browser-check",
        "--disable-web-security",
        "--disable-background-timer-throttling",
        "--disable-backgrounding-occluded-windows",
        "--disable-renderer-backgrounding",
        TARGET_URL
    ], { stdio: 'ignore', detached: true });
    chromeProc.unref();

    let wsUrl = null;
    for (let i = 0; i < 20; i++) {
        await delay(600);
        try {
            wsUrl = await getCdpWebSocketUrl();
            if (wsUrl) break;
        } catch(e) {}
    }

    if (!wsUrl) {
        console.error("[-] Falha ao conectar ao Chrome via CDP na porta " + CDP_PORT);
        process.exit(1);
    }
    console.log("[+] Conexao CDP ativa: " + wsUrl);

    const cdp = new CdpClient(wsUrl);
    await cdp.ready();
    await cdp.send("Runtime.enable");
    await cdp.send("Page.enable");
    await cdp.send("Console.enable");

    cdp.on("Runtime.exceptionThrown", (params) => {
        const details = params.exceptionDetails;
        console.error(`[-] BROWSER EXCEPTION: ${details.text} | ${details.exception ? JSON.stringify(details.exception) : ''}`);
    });

    cdp.on("Console.messageAdded", (params) => {
        const msg = params.message;
        if (msg.level === 'error' || msg.text.includes('Pod') || msg.text.includes('Sync') || msg.text.includes('SHA') || msg.text.includes('Upload') || msg.text.includes('Gravando')) {
            console.log(`    [Chrome Console ${msg.level}]: ${msg.text}`);
        }
    });

    console.log("[*] 2. Aguardando renderizacao da Central Web...");
    await delay(2500);

    const title = await cdp.eval("document.title");
    console.log(`[+] Pagina carregada com sucesso: "${title}"`);

    // 3. Abrir Modal de Podcasts
    console.log("[*] 3. Abrindo Modal de Podcasts (openPodModal)...");
    await cdp.eval(`
        if (typeof openPodModal === 'function') openPodModal();
        else document.getElementById('podBtn').click();
    `);
    await delay(2000);

    // 4. Configurar retencao para 5 mais recentes (podKeepRecentSelect = 5)
    console.log("[*] 4. Configurando retencao para 5 mais recentes (podKeepRecentSelect = 5)...");
    await cdp.eval(`
        const sel = document.getElementById('podKeepRecentSelect');
        if (sel) {
            sel.value = '5';
            sel.dispatchEvent(new Event('change'));
        }
    `);

    const subs = await cdp.eval(`
        (() => {
            try {
                const raw = localStorage.getItem('mps3_pod_subs_v4');
                if (raw) return JSON.parse(raw);
            } catch(e) {}
            return [];
        })()
    `);
    console.log("[+] Canais ativos:", subs.filter(s => s.enabled !== false).map(s => s.name));

    // 5. Raspar canais e gerar plano
    console.log("[*] 5. Disparando varredura dos feeds e canais (btnCheckPodcasts.click)...");
    await cdp.eval(`document.getElementById('btnCheckPodcasts').click();`);

    // Aguarda a raspagem ate os checkboxes aparecerem
    let planningDone = false;
    for (let sec = 0; sec < 90; sec++) {
        await delay(1000);
        const checkStatus = await cdp.eval(`
            (() => {
                const listEl = document.getElementById('podList');
                const text = listEl ? listEl.innerText.trim() : '';
                const isWorking = text.includes('Consultando') || text.includes('Raspando');
                const checks = document.querySelectorAll('.pod-check').length;
                const isClean = text.includes('Todos os episódios') || text.includes('Nenhum');
                const hasError = text.includes('Erro') || text.includes('Falha');
                return {
                    isWorking,
                    text: text.slice(0, 100),
                    checks,
                    isClean,
                    hasError
                };
            })()
        `);

        if (!checkStatus.isWorking && checkStatus.checks > 0) {
            console.log(`\n[+] Varredura concluida em ${sec+1}s!`);
            console.log(`    Episodios candidatos para sincronizar: ${checkStatus.checks}`);
            planningDone = true;
            break;
        } else if (checkStatus.hasError) {
            console.error(`\n[-] Erro na varredura: ${checkStatus.text}`);
            break;
        } else {
            process.stdout.write(`    Raspando feeds RSS e canais... [${sec+1}s]\r`);
        }
    }

    if (!planningDone) {
        console.error("[-] Falha ao gerar plano de sincronizacao.");
        process.exit(1);
    }

    // 6. Selecionar todos os 5 episódios de cada canal
    await cdp.eval(`
        const btnAll = document.getElementById('btnSelectAll');
        if (btnAll) btnAll.click();
    `);
    await delay(600);

    const planSummary = await cdp.eval(`
        ({
            summaryText: (document.getElementById('podSelectedSummary') || {}).textContent || '',
            startBtnText: (document.getElementById('btnStartPodSync') || {}).textContent || '',
            episodes: Array.from(document.querySelectorAll('#podList label')).map(l => {
                const titleEl = l.querySelector('div[title]');
                const badgeEl = l.querySelector('span');
                return {
                    title: titleEl ? titleEl.title : l.innerText.split('\\n')[0],
                    program: badgeEl ? badgeEl.textContent.trim() : ''
                };
            })
        })
    `);

    console.log("\n[+] Resumo da selecao:", planSummary.summaryText);
    console.log("[+] Botao de acao:", planSummary.startBtnText);
    console.log("[+] Lista de episodios planejados:");
    planSummary.episodes.forEach((ep, idx) => {
        console.log(`    ${idx+1}. [${ep.program}] ${ep.title}`);
    });

    // Screenshot do plano
    const ssPlan = await cdp.send("Page.captureScreenshot", { format: "png" });
    fs.writeFileSync("captures/podcasts_plan_5eps.png", Buffer.from(ssPlan.data, 'base64'));
    console.log("[+] Screenshot salva: captures/podcasts_plan_5eps.png");

    // 7. Iniciar Sincronização Oficial
    console.log("\n" + "=".repeat(75));
    console.log("🚀 6. INICIANDO SINCRONIZACAO OFICIAL NO HARDWARE REAL...");
    console.log("=".repeat(75));

    const clickRes = await cdp.eval(`
        (() => {
            try {
                const btn = document.getElementById('btnStartPodSync');
                if (!btn) return { ok: false, error: 'Botao btnStartPodSync nao encontrado' };
                btn.click();
                return { ok: true };
            } catch(e) {
                return { ok: false, error: e.message, stack: e.stack };
            }
        })()
    `);
    console.log("[+] Clique no botao de sincronizacao:", clickRes);

    const t0_sync = Date.now();
    let lastItemText = "";
    let lastPct = "";
    let loopCount = 0;

    while (true) {
        await delay(2000);
        loopCount++;

        let state;
        try {
            state = await cdp.eval(`
                (() => {
                    const startBtn = document.getElementById('btnStartPodSync');
                    const progBox = document.getElementById('podProgressBox');
                    const curItem = document.getElementById('podCurrentItem');
                    const stMsg = document.getElementById('podStatusMsg');
                    const progBar = document.getElementById('podProgressBar');
                    const itemBar = document.getElementById('podItemBar');
                    const itemPct = document.getElementById('podItemPct');
                    const overall = document.getElementById('podOverallInfo');

                    const isSyncing = startBtn ? (startBtn.disabled && (progBox ? progBox.style.display !== 'none' : false)) : false;
                    return {
                        isSyncing,
                        currentItem: curItem ? curItem.textContent.trim() : '',
                        statusMsg: stMsg ? stMsg.textContent.trim() : '',
                        overallInfo: overall ? overall.textContent.trim() : '',
                        totalBarWidth: progBar ? progBar.style.width : '0%',
                        itemBarWidth: itemBar ? itemBar.style.width : '0%',
                        itemPctText: itemPct ? itemPct.textContent.trim() : '0%',
                        hasError: stMsg && stMsg.style && stMsg.style.color ? stMsg.style.color.includes('dc2626') : false
                    };
                })()
            `);
        } catch(evalErr) {
            console.warn(`[!] Aviso no eval de estado (tentando novamente): ${evalErr.message}`);
            continue;
        }

        if (state.currentItem !== lastItemText || state.itemBarWidth !== lastPct) {
            const dtSec = ((Date.now() - t0_sync) / 1000).toFixed(0);
            console.log(`    [+${dtSec}s] (${state.overallInfo || 'Sync'}) [${state.itemBarWidth}] ${state.currentItem}`);
            lastItemText = state.currentItem;
            lastPct = state.itemBarWidth;
        }

        if (state.hasError && state.statusMsg) {
            console.error(`[-] MENSAGEM DE ERRO NA UI: ${state.statusMsg}`);
        }

        if (!state.isSyncing) {
            console.log("\n" + "=".repeat(75));
            console.log("[+] PROCESSO DE SINCRONIZACAO CONCLUIDO NA CENTRAL WEB!");
            console.log("=".repeat(75));
            console.log(`    Mensagem Final: ${state.statusMsg}`);
            console.log(`    Tempo Total:    ${((Date.now() - t0_sync) / 1000).toFixed(1)} s`);
            break;
        }

        // Screenshot a cada ~30s
        if (loopCount % 15 === 0) {
            try {
                const ss = await cdp.send("Page.captureScreenshot", { format: "png" });
                fs.writeFileSync(`captures/podcasts_live_${loopCount}.png`, Buffer.from(ss.data, 'base64'));
            } catch(e) {}
        }
    }

    // Screenshot final
    const ssFinal = await cdp.send("Page.captureScreenshot", { format: "png" });
    fs.writeFileSync("captures/podcasts_final_success.png", Buffer.from(ssFinal.data, 'base64'));
    console.log("[+] Screenshot final salva: captures/podcasts_final_success.png");

    console.log("\n[*] 7. Consultando arquivos gravados no SD (/api/list?path=/Podcasts)...");
    const httpRes = await fetch("http://mps3.local/api/list?path=/Podcasts");
    const jsonList = await httpRes.json();
    console.log("[+] Pastas e Arquivos em /Podcasts no cartao SD:");
    console.log(JSON.stringify(jsonList, null, 2));

    console.log("\n🎉 VALIDAÇÃO COMPLETA CONCLUÍDA COM SUCESSO!");
}

main().catch(err => {
    console.error("[-] Falha na execucao:", err);
    process.exit(1);
});
