/* ==========================================================================
 * MÓDULO JS 01 (HEAD): Bootstrap de Tema Claro/Escuro e Favicon SVG Adaptativo
 * Carregado no <head> para evitar Flash of Unstyled Content (FOUC).
 * ========================================================================== */
        /* --- Gerenciamento de Tema e Favicon Adaptativo --- */
        const FAV_AUTO = "data:image/svg+xml,%3Csvg%20xmlns%3D%22http%3A//www.w3.org/2000/svg%22%20viewBox%3D%220%200%201000%201000%22%3E%3Cstyle%3Epolygon%2Cpath%7Bstroke%3A%230077ff%3Btransition%3Astroke%200.3s%3B%7D%40media%28prefers-color-scheme%3Adark%29%7Bpolygon%2Cpath%7Bstroke%3A%23f5a623%3B%7D%7D%3C/style%3E%3Cg%20fill%3D%22none%22%20stroke-linecap%3D%22round%22%20stroke-linejoin%3D%22round%22%20stroke-width%3D%2244%22%3E%3Cpolygon%20points%3D%22500.00%2C60.00%20735.11%2C783.61%20119.58%2C336.39%20880.42%2C336.39%20264.89%2C783.61%22/%3E%3Cpath%20d%3D%22M%20500.00%2C60.00%20C%20671.49%2C131.35%20782.38%2C275.30%20816.25%2C417.34%20C%20850.12%2C559.38%20809.80%2C696.43%20735.11%2C783.61%20C%20660.42%2C870.78%20554.63%2C908.47%20464.67%2C901.25%20C%20374.72%2C894.03%20301.86%2C844.16%20264.89%2C783.61%20C%20227.91%2C723.06%20225.96%2C653.68%20247.39%2C602.18%20C%20268.81%2C550.68%20312.04%2C517.38%20354.69%2C507.21%20C%20397.34%2C497.04%20438.50%2C509.15%20464.67%2C531.58%20C%20490.85%2C554.01%20502.17%2C585.77%20500.00%2C612.79%20C%20497.83%2C639.80%20482.86%2C661.68%20464.67%2C672.78%20C%20446.49%2C683.88%20425.66%2C684.47%20410.19%2C678.03%20C%20394.73%2C671.60%20384.73%2C658.62%20381.68%2C645.81%20C%20378.62%2C633.00%20382.26%2C620.65%20388.99%2C612.79%20C%20395.73%2C604.93%20405.27%2C601.53%20413.38%2C602.18%20C%20421.49%2C602.83%20428.06%2C607.33%20431.39%2C612.79%20C%20434.73%2C618.25%20434.90%2C624.50%20432.97%2C629.15%20C%20431.04%2C633.79%20427.14%2C636.79%20423.30%2C637.71%20C%20419.45%2C638.63%20415.74%2C637.53%20413.38%2C635.51%20C%20411.02%2C633.49%20410.00%2C630.63%20410.19%2C628.19%20C%20410.39%2C625.75%20411.74%2C623.78%20413.38%2C622.78%20C%20415.02%2C621.78%20416.90%2C621.73%20418.29%2C622.31%20C%20419.69%2C622.89%20420.59%2C624.06%20420.86%2C625.21%20C%20421.14%2C626.37%20420.81%2C627.48%20420.20%2C628.19%20C%20419.60%2C628.90%20418.74%2C629.20%20418.00%2C629.15%20C%20417.27%2C629.09%20416.68%2C628.68%20416.38%2C628.19%20C%20416.08%2C627.70%20416.06%2C627.13%20416.24%2C626.71%20C%20416.41%2C626.30%20416.76%2C626.02%20417.11%2C625.94%20C%20417.46%2C625.86%20417.79%2C625.96%20418.00%2C626.14%20C%20418.22%2C626.32%20418.31%2C626.58%20418.29%2C626.80%20C%20418.27%2C627.02%20418.15%2C627.20%20418.00%2C627.29%20C%20417.86%2C627.38%20417.69%2C627.38%20417.56%2C627.33%20C%20417.44%2C627.28%20417.35%2C627.17%20417.33%2C627.07%20C%20417.31%2C626.96%20417.33%2C626.86%20417.39%2C626.80%20C%20417.44%2C626.74%20417.52%2C626.71%20417.59%2C626.71%20C%20417.65%2C626.72%20417.71%2C626.76%20417.73%2C626.80%20C%20417.76%2C626.84%20417.76%2C626.90%20417.75%2C626.93%20C%20417.73%2C626.97%20417.70%2C627.00%20417.67%2C627.00%20C%20417.64%2C627.01%20417.61%2C627.00%20417.59%2C626.99%20C%20417.57%2C626.97%20417.56%2C626.95%20417.56%2C626.93%20C%20417.56%2C626.91%20417.57%2C626.89%20417.59%2C626.88%20C%20417.60%2C626.87%20417.62%2C626.87%20417.63%2C626.88%20C%20417.64%2C626.88%20417.65%2C626.89%20417.65%2C626.90%20C%20417.65%2C626.91%20417.65%2C626.92%20417.64%2C626.93%20C%20417.64%2C626.93%20417.63%2C626.93%20417.63%2C626.93%20C%20417.62%2C626.93%20417.61%2C626.93%20417.61%2C626.93%22/%3E%3C/g%3E%3C/svg%3E";
        const FAV_DARK = "data:image/svg+xml,%3Csvg%20xmlns%3D%22http%3A//www.w3.org/2000/svg%22%20viewBox%3D%220%200%201000%201000%22%3E%3Cstyle%3Epolygon%2Cpath%7Bstroke%3A%23f5a623%3B%7D%3C/style%3E%3Cg%20fill%3D%22none%22%20stroke-linecap%3D%22round%22%20stroke-linejoin%3D%22round%22%20stroke-width%3D%2244%22%3E%3Cpolygon%20points%3D%22500.00%2C60.00%20735.11%2C783.61%20119.58%2C336.39%20880.42%2C336.39%20264.89%2C783.61%22/%3E%3Cpath%20d%3D%22M%20500.00%2C60.00%20C%20671.49%2C131.35%20782.38%2C275.30%20816.25%2C417.34%20C%20850.12%2C559.38%20809.80%2C696.43%20735.11%2C783.61%20C%20660.42%2C870.78%20554.63%2C908.47%20464.67%2C901.25%20C%20374.72%2C894.03%20301.86%2C844.16%20264.89%2C783.61%20C%20227.91%2C723.06%20225.96%2C653.68%20247.39%2C602.18%20C%20268.81%2C550.68%20312.04%2C517.38%20354.69%2C507.21%20C%20397.34%2C497.04%20438.50%2C509.15%20464.67%2C531.58%20C%20490.85%2C554.01%20502.17%2C585.77%20500.00%2C612.79%20C%20497.83%2C639.80%20482.86%2C661.68%20464.67%2C672.78%20C%20446.49%2C683.88%20425.66%2C684.47%20410.19%2C678.03%20C%20394.73%2C671.60%20384.73%2C658.62%20381.68%2C645.81%20C%20378.62%2C633.00%20382.26%2C620.65%20388.99%2C612.79%20C%20395.73%2C604.93%20405.27%2C601.53%20413.38%2C602.18%20C%20421.49%2C602.83%20428.06%2C607.33%20431.39%2C612.79%20C%20434.73%2C618.25%20434.90%2C624.50%20432.97%2C629.15%20C%20431.04%2C633.79%20427.14%2C636.79%20423.30%2C637.71%20C%20419.45%2C638.63%20415.74%2C637.53%20413.38%2C635.51%20C%20411.02%2C633.49%20410.00%2C630.63%20410.19%2C628.19%20C%20410.39%2C625.75%20411.74%2C623.78%20413.38%2C622.78%20C%20415.02%2C621.78%20416.90%2C621.73%20418.29%2C622.31%20C%20419.69%2C622.89%20420.59%2C624.06%20420.86%2C625.21%20C%20421.14%2C626.37%20420.81%2C627.48%20420.20%2C628.19%20C%20419.60%2C628.90%20418.74%2C629.20%20418.00%2C629.15%20C%20417.27%2C629.09%20416.68%2C628.68%20416.38%2C628.19%20C%20416.08%2C627.70%20416.06%2C627.13%20416.24%2C626.71%20C%20416.41%2C626.30%20416.76%2C626.02%20417.11%2C625.94%20C%20417.46%2C625.86%20417.79%2C625.96%20418.00%2C626.14%20C%20418.22%2C626.32%20418.31%2C626.58%20418.29%2C626.80%20C%20418.27%2C627.02%20418.15%2C627.20%20418.00%2C627.29%20C%20417.86%2C627.38%20417.69%2C627.38%20417.56%2C627.33%20C%20417.44%2C627.28%20417.35%2C627.17%20417.33%2C627.07%20C%20417.31%2C626.96%20417.33%2C626.86%20417.39%2C626.80%20C%20417.44%2C626.74%20417.52%2C626.71%20417.59%2C626.71%20C%20417.65%2C626.72%20417.71%2C626.76%20417.73%2C626.80%20C%20417.76%2C626.84%20417.76%2C626.90%20417.75%2C626.93%20C%20417.73%2C626.97%20417.70%2C627.00%20417.67%2C627.00%20C%20417.64%2C627.01%20417.61%2C627.00%20417.59%2C626.99%20C%20417.57%2C626.97%20417.56%2C626.95%20417.56%2C626.93%20C%20417.56%2C626.91%20417.57%2C626.89%20417.59%2C626.88%20C%20417.60%2C626.87%20417.62%2C626.87%20417.63%2C626.88%20C%20417.64%2C626.88%20417.65%2C626.89%20417.65%2C626.90%20C%20417.65%2C626.91%20417.65%2C626.92%20417.64%2C626.93%20C%20417.64%2C626.93%20417.63%2C626.93%20417.63%2C626.93%20C%20417.62%2C626.93%20417.61%2C626.93%20417.61%2C626.93%22/%3E%3C/g%3E%3C/svg%3E";
        const FAV_LIGHT = "data:image/svg+xml,%3Csvg%20xmlns%3D%22http%3A//www.w3.org/2000/svg%22%20viewBox%3D%220%200%201000%201000%22%3E%3Cstyle%3Epolygon%2Cpath%7Bstroke%3A%230077ff%3B%7D%3C/style%3E%3Cg%20fill%3D%22none%22%20stroke-linecap%3D%22round%22%20stroke-linejoin%3D%22round%22%20stroke-width%3D%2244%22%3E%3Cpolygon%20points%3D%22500.00%2C60.00%20735.11%2C783.61%20119.58%2C336.39%20880.42%2C336.39%20264.89%2C783.61%22/%3E%3Cpath%20d%3D%22M%20500.00%2C60.00%20C%20671.49%2C131.35%20782.38%2C275.30%20816.25%2C417.34%20C%20850.12%2C559.38%20809.80%2C696.43%20735.11%2C783.61%20C%20660.42%2C870.78%20554.63%2C908.47%20464.67%2C901.25%20C%20374.72%2C894.03%20301.86%2C844.16%20264.89%2C783.61%20C%20227.91%2C723.06%20225.96%2C653.68%20247.39%2C602.18%20C%20268.81%2C550.68%20312.04%2C517.38%20354.69%2C507.21%20C%20397.34%2C497.04%20438.50%2C509.15%20464.67%2C531.58%20C%20490.85%2C554.01%20502.17%2C585.77%20500.00%2C612.79%20C%20497.83%2C639.80%20482.86%2C661.68%20464.67%2C672.78%20C%20446.49%2C683.88%20425.66%2C684.47%20410.19%2C678.03%20C%20394.73%2C671.60%20384.73%2C658.62%20381.68%2C645.81%20C%20378.62%2C633.00%20382.26%2C620.65%20388.99%2C612.79%20C%20395.73%2C604.93%20405.27%2C601.53%20413.38%2C602.18%20C%20421.49%2C602.83%20428.06%2C607.33%20431.39%2C612.79%20C%20434.73%2C618.25%20434.90%2C624.50%20432.97%2C629.15%20C%20431.04%2C633.79%20427.14%2C636.79%20423.30%2C637.71%20C%20419.45%2C638.63%20415.74%2C637.53%20413.38%2C635.51%20C%20411.02%2C633.49%20410.00%2C630.63%20410.19%2C628.19%20C%20410.39%2C625.75%20411.74%2C623.78%20413.38%2C622.78%20C%20415.02%2C621.78%20416.90%2C621.73%20418.29%2C622.31%20C%20419.69%2C622.89%20420.59%2C624.06%20420.86%2C625.21%20C%20421.14%2C626.37%20420.81%2C627.48%20420.20%2C628.19%20C%20419.60%2C628.90%20418.74%2C629.20%20418.00%2C629.15%20C%20417.27%2C629.09%20416.68%2C628.68%20416.38%2C628.19%20C%20416.08%2C627.70%20416.06%2C627.13%20416.24%2C626.71%20C%20416.41%2C626.30%20416.76%2C626.02%20417.11%2C625.94%20C%20417.46%2C625.86%20417.79%2C625.96%20418.00%2C626.14%20C%20418.22%2C626.32%20418.31%2C626.58%20418.29%2C626.80%20C%20418.27%2C627.02%20418.15%2C627.20%20418.00%2C627.29%20C%20417.86%2C627.38%20417.69%2C627.38%20417.56%2C627.33%20C%20417.44%2C627.28%20417.35%2C627.17%20417.33%2C627.07%20C%20417.31%2C626.96%20417.33%2C626.86%20417.39%2C626.80%20C%20417.44%2C626.74%20417.52%2C626.71%20417.59%2C626.71%20C%20417.65%2C626.72%20417.71%2C626.76%20417.73%2C626.80%20C%20417.76%2C626.84%20417.76%2C626.90%20417.75%2C626.93%20C%20417.73%2C626.97%20417.70%2C627.00%20417.67%2C627.00%20C%20417.64%2C627.01%20417.61%2C627.00%20417.59%2C626.99%20C%20417.57%2C626.97%20417.56%2C626.95%20417.56%2C626.93%20C%20417.56%2C626.91%20417.57%2C626.89%20417.59%2C626.88%20C%20417.60%2C626.87%20417.62%2C626.87%20417.63%2C626.88%20C%20417.64%2C626.88%20417.65%2C626.89%20417.65%2C626.90%20C%20417.65%2C626.91%20417.65%2C626.92%20417.64%2C626.93%20C%20417.64%2C626.93%20417.63%2C626.93%20417.63%2C626.93%20C%20417.62%2C626.93%20417.61%2C626.93%20417.61%2C626.93%22/%3E%3C/g%3E%3C/svg%3E";

        function updateFaviconAndUI(theme, isDark) {
            const faviconEl = document.getElementById('siteFavicon');
            const toggleBtn = document.getElementById('themeToggleBtn');
            const btnAuto = document.getElementById('themeBtnAuto');
            const btnDark = document.getElementById('themeBtnDark');
            const btnLight = document.getElementById('themeBtnLight');

            if (isDark) {
                document.body.classList.add('dark-theme');
                document.documentElement.classList.add('dark-theme');
                if (faviconEl) faviconEl.href = (theme === 'auto') ? FAV_AUTO : FAV_DARK;
                if (toggleBtn) toggleBtn.textContent = '☀️';
            } else {
                document.body.classList.remove('dark-theme');
                document.documentElement.classList.remove('dark-theme');
                if (faviconEl) faviconEl.href = (theme === 'auto') ? FAV_AUTO : FAV_LIGHT;
                if (toggleBtn) toggleBtn.textContent = '🌙';
            }

            if (btnAuto && btnDark && btnLight) {
                btnAuto.style.borderColor = (theme === 'auto') ? 'var(--accent-color)' : '';
                btnAuto.style.fontWeight = (theme === 'auto') ? 'bold' : 'normal';
                btnDark.style.borderColor = (theme === 'dark') ? 'var(--accent-color)' : '';
                btnDark.style.fontWeight = (theme === 'dark') ? 'bold' : 'normal';
                btnLight.style.borderColor = (theme === 'light') ? 'var(--accent-color)' : '';
                btnLight.style.fontWeight = (theme === 'light') ? 'bold' : 'normal';
            }
        }

        function setTheme(theme) {
            localStorage.setItem('mps3_theme', theme);
            const isDark = (theme === 'dark') || (theme === 'auto' && window.matchMedia && window.matchMedia('(prefers-color-scheme: dark)').matches);
            updateFaviconAndUI(theme, isDark);
        }

        function toggleTheme() {
            const curTheme = localStorage.getItem('mps3_theme') || 'auto';
            const isCurrentlyDark = document.body.classList.contains('dark-theme');
            setTheme(isCurrentlyDark ? 'light' : 'dark');
        }

        // Listener para mudanca dinamica de tema no sistema operacional
        if (window.matchMedia) {
            window.matchMedia('(prefers-color-scheme: dark)').addEventListener('change', e => {
                const savedTheme = localStorage.getItem('mps3_theme') || 'auto';
                if (savedTheme === 'auto') {
                    updateFaviconAndUI('auto', e.matches);
                }
            });
        }

        function setOledColor(color, syncStudio) {
            const valid = ['white', 'blue', 'yellow', 'green', 'dual'];
            const c = valid.includes(color) ? color : 'blue';
            localStorage.setItem('mps3_oled_color', c);
            document.documentElement.setAttribute('data-oled-color', c);
            if (document.body) document.body.setAttribute('data-oled-color', c);
            document.querySelectorAll('.oled-swatch').forEach(btn => {
                btn.classList.toggle('active', btn.getAttribute('data-oled') === c);
            });
            if (typeof window.renderHeaderOled128x64 === 'function') {
                window.renderHeaderOled128x64();
            }
            if (syncStudio !== false && typeof window.syncWrappedOledColorFromSite === 'function') {
                window.syncWrappedOledColorFromSite(c);
            }
        }

        // Inicializa estado dos botoes na carga da pagina
        window.addEventListener('DOMContentLoaded', () => {
            const savedTheme = localStorage.getItem('mps3_theme') || 'auto';
            const isDark = savedTheme === 'dark' || (savedTheme === 'auto' && window.matchMedia && window.matchMedia('(prefers-color-scheme: dark)').matches);
            updateFaviconAndUI(savedTheme, isDark);
            const savedOled = localStorage.getItem('mps3_oled_color') || 'blue';
            setOledColor(savedOled, false);
            if (typeof window.updateCloudStatusUI === 'function') {
                window.updateCloudStatusUI();
            }
        });

        /* ======================================================================
         * DESACOPLAMENTO DE API (?mps3=<IP>) & CLOUD HOT-SWAP (GitHub Pages CDN)
         * ====================================================================== */
        window.MPS3_WEB_BUILD_HASH = "__MPS3_WEB_BUILD_HASH__";
        window.MPS3_DEFAULT_CLOUD_URL = "https://thall-is.github.io/mps3-web";
        window.MPS3_FALLBACK_RAW_URL = "https://raw.githubusercontent.com/thall-is/mps3-web/main";

        (function initMps3DecoupledTransport() {
            // 1. Resolução do Host da API (?mps3=mps3.local ou sessionStorage)
            let apiBase = "";
            try {
                const params = new URLSearchParams(window.location.search);
                const qHost = params.get("mps3");
                if (qHost) {
                    if (qHost === "local" || qHost === "reset" || qHost === "same") {
                        sessionStorage.removeItem("mps3_api_base");
                    } else {
                        let clean = qHost.trim().replace(/\/+$/, "");
                        if (!/^https?:\/\//i.test(clean)) {
                            clean = "http://" + clean;
                        }
                        sessionStorage.setItem("mps3_api_base", clean);
                    }
                }
                apiBase = sessionStorage.getItem("mps3_api_base") || "";
            } catch (e) {}

            window.MPS3_API_BASE = apiBase;
            window.getMps3ApiBase = function() {
                return window.MPS3_API_BASE || "";
            };
            window.resolveMps3Url = function(url) {
                if (typeof url === "string" && url.startsWith("/api/") && window.MPS3_API_BASE) {
                    return window.MPS3_API_BASE + url;
                }
                return url;
            };

            // 2. Interceptador Transparente de window.fetch para caminhos /api/*
            const origFetch = window.fetch.bind(window);
            window.fetch = function(input, init) {
                if (typeof input === "string" && input.startsWith("/api/") && window.MPS3_API_BASE) {
                    input = window.MPS3_API_BASE + input;
                } else if (input && typeof input === "object" && typeof input.url === "string" && window.MPS3_API_BASE) {
                    try {
                        const u = new URL(input.url, window.location.origin);
                        if (u.origin === window.location.origin && u.pathname.startsWith("/api/")) {
                            input = window.MPS3_API_BASE + u.pathname + u.search;
                        }
                    } catch (e) {}
                }
                return origFetch(input, init);
            };

            // 3. Interceptador Transparente de XMLHttpRequest.prototype.open para /api/*
            const origXhrOpen = XMLHttpRequest.prototype.open;
            XMLHttpRequest.prototype.open = function(method, url, async, user, password) {
                if (typeof url === "string" && url.startsWith("/api/") && window.MPS3_API_BASE) {
                    url = window.MPS3_API_BASE + url;
                }
                return origXhrOpen.call(this, method, url, async !== undefined ? async : true, user, password);
            };

            // 4. Observador leve de DOM para reescrever src/href relativos (/api/download, /api/qr) quando desacoplado
            if (window.MPS3_API_BASE && typeof MutationObserver !== "undefined") {
                const rewriteNodeAttrs = (root) => {
                    if (!root || !root.querySelectorAll || !window.MPS3_API_BASE) return;
                    root.querySelectorAll("img[src^='/api/'], audio[src^='/api/'], video[src^='/api/'], a[href^='/api/']").forEach(el => {
                        if (el.hasAttribute("src")) {
                            const s = el.getAttribute("src");
                            if (s && s.startsWith("/api/")) el.setAttribute("src", window.MPS3_API_BASE + s);
                        }
                        if (el.hasAttribute("href")) {
                            const h = el.getAttribute("href");
                            if (h && h.startsWith("/api/")) el.setAttribute("href", window.MPS3_API_BASE + h);
                        }
                    });
                };
                window.addEventListener("DOMContentLoaded", () => {
                    rewriteNodeAttrs(document.body);
                    const obs = new MutationObserver((mutations) => {
                        for (const m of mutations) {
                            if (m.type === "attributes" && m.target) {
                                const attr = m.attributeName;
                                const val = m.target.getAttribute && m.target.getAttribute(attr);
                                if (val && val.startsWith("/api/") && window.MPS3_API_BASE) {
                                    m.target.setAttribute(attr, window.MPS3_API_BASE + val);
                                }
                            } else if (m.addedNodes && m.addedNodes.length) {
                                m.addedNodes.forEach(n => {
                                    if (n.nodeType === 1) {
                                        if (n.getAttribute) {
                                            const s = n.getAttribute("src");
                                            if (s && s.startsWith("/api/")) n.setAttribute("src", window.MPS3_API_BASE + s);
                                            const h = n.getAttribute("href");
                                            if (h && h.startsWith("/api/")) n.setAttribute("href", window.MPS3_API_BASE + h);
                                        }
                                        rewriteNodeAttrs(n);
                                    }
                                });
                            }
                        }
                    });
                    obs.observe(document.documentElement, { childList: true, subtree: true, attributes: true, attributeFilter: ["src", "href"] });
                });
            }
        })();

        // --- Limpeza Proativa de Cache de Versões Anteriores ---
        try {
            if (typeof indexedDB !== "undefined") {
                indexedDB.deleteDatabase("mps3_cloud_ui_cache");
            }
        } catch (e) {}

        // --- Motor de Verificação de Atualização via GitHub Pages (CDN) ---
        function openCloudUiDb() {
            return new Promise((resolve) => {
                if (typeof indexedDB === "undefined") return resolve(null);
                try {
                    const req = indexedDB.open("mps3_cloud_ui_cache", 1);
                    req.onupgradeneeded = (e) => {
                        const db = e.target.result;
                        if (!db.objectStoreNames.contains("bundles")) {
                            db.createObjectStore("bundles", { keyPath: "id" });
                        }
                    };
                    req.onsuccess = (e) => resolve(e.target.result);
                    req.onerror = () => resolve(null);
                } catch (e) {
                    resolve(null);
                }
            });
        }

        async function getCachedCloudBundle() {
            const db = await openCloudUiDb();
            if (!db) return null;
            return new Promise((resolve) => {
                try {
                    const tx = db.transaction("bundles", "readonly");
                    const req = tx.objectStore("bundles").get("latest");
                    req.onsuccess = () => resolve(req.result || null);
                    req.onerror = () => resolve(null);
                } catch (e) {
                    resolve(null);
                }
            });
        }

        async function saveCachedCloudBundle(hash, html, builtAt) {
            const db = await openCloudUiDb();
            if (!db) return;
            return new Promise((resolve) => {
                try {
                    const tx = db.transaction("bundles", "readwrite");
                    tx.objectStore("bundles").put({
                        id: "latest",
                        hash: hash,
                        html: html,
                        builtAt: builtAt || new Date().toISOString(),
                        savedAt: Date.now()
                    });
                    tx.oncomplete = () => resolve(true);
                    tx.onerror = () => resolve(false);
                } catch (e) {
                    resolve(false);
                }
            });
        }

        window.updateCloudStatusUI = function(customMsg, isHtml) {
            const badge = document.getElementById("cloudSyncStatusBadge");
            if (!badge) return;
            const isCloudBooted = !!window.__MPS3_CLOUD_BOOTED__;
            const curHash = window.MPS3_WEB_BUILD_HASH || "dev";
            const apiTarget = window.MPS3_API_BASE ? `API: ${window.MPS3_API_BASE}` : "API: Local (Same-Origin)";
            const srcLabel = isCloudBooted ? `☁️ Cloud CDN (#${curHash.slice(0, 8)})` : `⚡ Flash Embarcada (#${curHash.slice(0, 8)})`;
            if (isHtml) {
                badge.innerHTML = `${srcLabel} • ${customMsg}`;
            } else {
                badge.textContent = customMsg ? `${srcLabel} • ${customMsg}` : `${srcLabel} • ${apiTarget}`;
            }
        };

        window.checkCloudWebUpdate = async function(manualTrigger) {
            const cloudUrl = (localStorage.getItem("mps3_cloud_url") || window.MPS3_DEFAULT_CLOUD_URL).replace(/\/+$/, "");
            // No modo Hotspot AP (192.168.4.1), nunca tenta acessar a internet automaticamente
            if (!manualTrigger && window.location.hostname === "192.168.4.1") {
                return false;
            }
            if (!manualTrigger && localStorage.getItem("mps3_cloud_sync") === "0") {
                return false;
            }
            if (manualTrigger) {
                window.updateCloudStatusUI("Verificando GitHub Pages...");
            }

            try {
                let activeBase = cloudUrl;
                const fetchManifest = async (baseUrl, timeoutMs) => {
                    const ctrl = new AbortController();
                    const timer = setTimeout(() => ctrl.abort(), timeoutMs);
                    try {
                        const r = await fetch(`${baseUrl}/version.json?t=${Date.now()}`, {
                            signal: ctrl.signal,
                            cache: "no-store"
                        });
                        clearTimeout(timer);
                        return r;
                    } catch (e) {
                        clearTimeout(timer);
                        throw e;
                    }
                };

                let verRes = null;
                try {
                    verRes = await fetchManifest(activeBase, manualTrigger ? 5000 : 2200);
                } catch (e) {
                    verRes = null;
                }

                // Fallback automático para raw.githubusercontent.com caso GitHub Pages ainda não esteja ativo
                if ((!verRes || !verRes.ok) && window.MPS3_FALLBACK_RAW_URL && activeBase === window.MPS3_DEFAULT_CLOUD_URL) {
                    activeBase = window.MPS3_FALLBACK_RAW_URL;
                    verRes = await fetchManifest(activeBase, manualTrigger ? 5000 : 2200);
                }

                if (!verRes || !verRes.ok) {
                    if (manualTrigger) window.updateCloudStatusUI(`CDN indisponível (HTTP ${verRes ? verRes.status : 'ERR'} - verifique se o repositório é público)`);
                    return false;
                }

                const manifest = await verRes.json();
                const remoteHash = (manifest && manifest.hash) ? String(manifest.hash).trim() : "";
                if (!remoteHash) {
                    if (manualTrigger) window.updateCloudStatusUI("Manifesto remoto inválido");
                    return false;
                }

                if (remoteHash === window.MPS3_WEB_BUILD_HASH) {
                    if (manualTrigger) window.updateCloudStatusUI("Central Web já está na versão mais recente!");
                    return false;
                }

                if (manualTrigger) {
                    const targetIp = window.location.hostname || "192.168.4.1";
                    const cloudUrlFull = `${activeBase}/?mps3=${encodeURIComponent(targetIp)}`;
                    window.updateCloudStatusUI(
                        `<a href="${cloudUrlFull}" target="_blank" rel="noopener" style="color:var(--accent-color);font-weight:bold;text-decoration:underline;">Abrir versão #${remoteHash.slice(0, 8)} na Nuvem ↗</a>`,
                        true
                    );
                }
                return true;
            } catch (err) {
                if (manualTrigger) {
                    window.updateCloudStatusUI("Offline ou GitHub Pages ainda não publicado");
                }
                return false;
            }
        };

        window.resetCloudWebCache = async function() {
            const db = await openCloudUiDb();
            if (db) {
                await new Promise((res) => {
                    try {
                        const tx = db.transaction("bundles", "readwrite");
                        tx.objectStore("bundles").clear();
                        tx.oncomplete = () => res(true);
                        tx.onerror = () => res(false);
                    } catch (e) {
                        res(false);
                    }
                });
            }
            window.__MPS3_CLOUD_BOOTED__ = false;
            window.location.reload();
        };

        (function() {
            var theme = localStorage.getItem('mps3_theme') || 'auto';
            var isDark = theme === 'dark' || (theme === 'auto' && window.matchMedia && window.matchMedia('(prefers-color-scheme: dark)').matches);
            var oledColor = localStorage.getItem('mps3_oled_color') || 'blue';
            document.documentElement.setAttribute('data-oled-color', oledColor);
            if (isDark) {
                document.documentElement.classList.add('dark-theme');
                document.addEventListener('DOMContentLoaded', function() {
                    if (document.body) {
                        document.body.classList.add('dark-theme');
                        document.body.setAttribute('data-oled-color', oledColor);
                    }
                });
            }
        })();
