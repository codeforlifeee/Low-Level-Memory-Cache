#include <cstdlib>
#include <chrono>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

#include <httplib.h>

#include "mini_redis/command_processor.hpp"

namespace {

std::optional<std::chrono::milliseconds> parse_ttl_ms(const httplib::Request& req) {
    if (!req.has_param("ttl_ms")) {
        return std::nullopt;
    }

    const auto raw = req.get_param_value("ttl_ms");
    std::size_t pos = 0;
    const long long ttl_ms = std::stoll(raw, &pos);
    if (pos != raw.size() || ttl_ms < 0) {
        throw std::runtime_error("ttl_ms must be a non-negative integer");
    }

    return std::chrono::milliseconds(ttl_ms);
}

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c; break;
        }
    }
    return out;
}

std::string json_ok(const std::string& message) {
    return std::string{"{\"ok\":true,\"message\":\""} + json_escape(message) + "\"}";
}

std::string json_err(const std::string& message) {
    return std::string{"{\"ok\":false,\"error\":\""} + json_escape(message) + "\"}";
}

const char* ui_html() {
        return R"HTML(
<!doctype html>
<html lang="en">
<head>
    <meta charset="utf-8" />
    <meta name="viewport" content="width=device-width, initial-scale=1" />
    <title>Mini Redis UI</title>
    <style>
        :root {
            --bg-1: #eef6ff;
            --bg-2: #fffaf2;
            --card: #ffffff;
            --text: #0f172a;
            --muted: #64748b;
            --accent: #0f766e;
            --accent-2: #0369a1;
            --danger: #b91c1c;
            --ok: #166534;
            --warn: #92400e;
            --border: #dbe4ee;
            --code-bg: #f8fafc;
        }
        * { box-sizing: border-box; }
        body {
            margin: 0;
            font-family: "Trebuchet MS", "Segoe UI", Tahoma, sans-serif;
            color: var(--text);
            background:
                radial-gradient(1200px 520px at -10% -10%, var(--bg-1), transparent 60%),
                radial-gradient(1000px 500px at 110% 0%, var(--bg-2), transparent 60%),
                #f8fafc;
            min-height: 100vh;
            padding: 18px;
        }
        .wrap {
            max-width: 1100px;
            margin: 0 auto;
            display: grid;
            gap: 12px;
        }
        .card {
            background: var(--card);
            border: 1px solid var(--border);
            border-radius: 14px;
            padding: 14px;
            box-shadow: 0 10px 24px rgba(2, 8, 23, 0.06);
        }
        h1 {
            margin: 0;
            font-size: 24px;
            letter-spacing: 0.2px;
        }
        p {
            margin: 6px 0 0;
            color: var(--muted);
        }
        .headline {
            display: flex;
            align-items: center;
            justify-content: space-between;
            gap: 10px;
            flex-wrap: wrap;
        }
        .badge {
            font-size: 12px;
            font-weight: 700;
            letter-spacing: 0.3px;
            padding: 7px 10px;
            border-radius: 999px;
            border: 1px solid #c7d2fe;
            background: #eef2ff;
            color: #3730a3;
        }
        .grid-main {
            display: grid;
            gap: 12px;
            grid-template-columns: 1.4fr 1fr;
        }
        .stats-grid {
            margin-top: 10px;
            display: grid;
            grid-template-columns: repeat(2, minmax(0, 1fr));
            gap: 8px;
        }
        .stat {
            border: 1px solid var(--border);
            border-radius: 10px;
            padding: 10px;
            background: #fcfdff;
        }
        .stat .k {
            color: var(--muted);
            font-size: 12px;
        }
        .stat .v {
            margin-top: 3px;
            font-size: 18px;
            font-weight: 700;
            font-family: Consolas, monospace;
        }
        .stat.good .v { color: var(--ok); }
        .stat.warn .v { color: var(--warn); }
        .row { display: grid; grid-template-columns: 1fr 1fr 1fr auto; gap: 8px; margin-top: 10px; }
        .row-2 { display: grid; grid-template-columns: 1fr 1fr; gap: 8px; margin-top: 8px; }
        .row-3 { display: grid; grid-template-columns: 1fr auto; gap: 8px; margin-top: 10px; }
        input, textarea, button {
            border-radius: 10px;
            border: 1px solid var(--border);
            padding: 10px;
            font: inherit;
        }
        textarea { width: 100%; min-height: 80px; resize: vertical; }
        input, textarea {
            background: #ffffff;
            transition: border-color 0.2s ease, box-shadow 0.2s ease;
        }
        input:focus, textarea:focus {
            outline: none;
            border-color: #93c5fd;
            box-shadow: 0 0 0 3px rgba(59, 130, 246, 0.15);
        }
        button {
            background: linear-gradient(135deg, var(--accent), var(--accent-2));
            color: white;
            border: none;
            cursor: pointer;
            font-weight: 600;
            transition: transform 0.1s ease, opacity 0.2s ease;
        }
        button:hover { opacity: 0.95; }
        button:active { transform: translateY(1px); }
        button.danger { background: var(--danger); }
        .out {
            margin-top: 10px;
            padding: 10px;
            background: var(--code-bg);
            border: 1px solid var(--border);
            border-radius: 10px;
            white-space: pre-wrap;
            font-family: Consolas, monospace;
            min-height: 56px;
        }
        .history-wrap {
            margin-top: 10px;
            border: 1px solid var(--border);
            border-radius: 10px;
            overflow: hidden;
        }
        table {
            width: 100%;
            border-collapse: collapse;
            background: white;
        }
        th, td {
            border-bottom: 1px solid #e5edf7;
            padding: 8px;
            text-align: left;
            font-size: 12px;
            vertical-align: top;
        }
        th {
            background: #f1f5f9;
            color: #334155;
            font-weight: 700;
            letter-spacing: 0.2px;
        }
        .chip {
            display: inline-block;
            font-size: 11px;
            padding: 2px 8px;
            border-radius: 999px;
            font-weight: 700;
        }
        .chip.ok { background: #dcfce7; color: #166534; }
        .chip.err { background: #fee2e2; color: #991b1b; }
        .muted { color: var(--muted); font-size: 12px; }
        @media (max-width: 780px) {
            .grid-main { grid-template-columns: 1fr; }
            .row, .row-2, .row-3, .stats-grid { grid-template-columns: 1fr; }
        }
    </style>
</head>
<body>
    <div class="wrap">
        <div class="card">
            <div class="headline">
                <div>
                    <h1>Mini Redis Dashboard</h1>
                    <p>Visual command center for your in-memory cache server.</p>
                </div>
                <span id="healthBadge" class="badge">Checking health...</span>
            </div>
        </div>

        <div class="grid-main">
            <div class="card">
                <strong>Key-Value Operations</strong>
                <div class="muted">Use key + value + optional ttl in milliseconds.</div>
                <div class="row">
                    <input id="key" placeholder="key" />
                    <input id="value" placeholder="value" />
                    <input id="ttl" placeholder="ttl_ms (optional)" />
                    <button onclick="setValue()">SET</button>
                </div>
                <div class="row-2">
                    <button onclick="getValue()">GET</button>
                    <button class="danger" onclick="delValue()">DEL</button>
                </div>
                <div id="kvOut" class="out"></div>

                <strong style="display:block;margin-top:12px;">Raw Command (POST /cmd)</strong>
                <textarea id="cmd" placeholder="Examples:&#10;SET user:1 Alice 3000&#10;GET user:1&#10;STATS"></textarea>
                <div class="row-3">
                    <button onclick="runCmd()">RUN COMMAND</button>
                    <button onclick="ping()">PING</button>
                </div>
                <div id="cmdOut" class="out"></div>
            </div>

            <div class="card">
                <strong>Live Stats</strong>
                <div class="muted">Auto-refresh every 2 seconds.</div>
                <div class="stats-grid">
                    <div class="stat"><div class="k">hits</div><div class="v" id="s_hits">-</div></div>
                    <div class="stat"><div class="k">misses</div><div class="v" id="s_misses">-</div></div>
                    <div class="stat good"><div class="k">hit_ratio</div><div class="v" id="s_ratio">-</div></div>
                    <div class="stat"><div class="k">active_keys</div><div class="v" id="s_active">-</div></div>
                    <div class="stat"><div class="k">memory_bytes</div><div class="v" id="s_mem">-</div></div>
                    <div class="stat warn"><div class="k">evictions</div><div class="v" id="s_evict">-</div></div>
                    <div class="stat"><div class="k">expirations</div><div class="v" id="s_exp">-</div></div>
                    <div class="stat"><div class="k">server_status</div><div class="v" id="s_status">-</div></div>
                </div>
                <div id="statsRaw" class="out" style="min-height: 42px;"></div>
            </div>
        </div>

        <div class="card">
            <div class="headline">
                <strong>Last 20 Operations</strong>
                <button onclick="clearHistory()" style="max-width:160px;">Clear History</button>
            </div>
            <div class="history-wrap">
                <table>
                    <thead>
                        <tr>
                            <th style="width:110px;">Time</th>
                            <th style="width:100px;">Action</th>
                            <th>Details</th>
                            <th style="width:90px;">Status</th>
                        </tr>
                    </thead>
                    <tbody id="historyBody">
                        <tr><td colspan="4" class="muted">No operations yet.</td></tr>
                    </tbody>
                </table>
            </div>
        </div>
    </div>

    <script>
        const kvOut = document.getElementById('kvOut');
        const statsOut = document.getElementById('statsOut');
        const cmdOut = document.getElementById('cmdOut');
        const historyBody = document.getElementById('historyBody');
        const healthBadge = document.getElementById('healthBadge');
        const history = [];

        function timeNow() {
            return new Date().toLocaleTimeString();
        }

        function fmtNum(v) {
            return Number(v || 0).toLocaleString();
        }

        function addHistory(action, details, ok) {
            history.unshift({ t: timeNow(), action, details, ok });
            if (history.length > 20) history.length = 20;
            renderHistory();
        }

        function renderHistory() {
            if (!history.length) {
                historyBody.innerHTML = '<tr><td colspan="4" class="muted">No operations yet.</td></tr>';
                return;
            }
            historyBody.innerHTML = history.map(item => {
                const status = item.ok ? '<span class="chip ok">OK</span>' : '<span class="chip err">ERR</span>';
                return `<tr><td>${item.t}</td><td>${item.action}</td><td>${String(item.details).replace(/</g, '&lt;')}</td><td>${status}</td></tr>`;
            }).join('');
        }

        function clearHistory() {
            history.length = 0;
            renderHistory();
        }

        function key() { return document.getElementById('key').value; }
        function value() { return document.getElementById('value').value; }
        function ttl() { return document.getElementById('ttl').value; }

        async function setValue() {
            try {
                const q = new URLSearchParams({ key: key(), value: value() });
                if (ttl()) q.append('ttl_ms', ttl());
                const r = await fetch('/set?' + q.toString(), { method: 'POST' });
                const txt = await r.text();
                kvOut.textContent = txt;
                addHistory('SET', `key=${key()} ttl=${ttl() || '-'} -> ${txt}`, r.ok);
                refreshStats();
            } catch (e) {
                kvOut.textContent = String(e);
                addHistory('SET', String(e), false);
            }
        }

        async function getValue() {
            try {
                const q = new URLSearchParams({ key: key() });
                const r = await fetch('/get?' + q.toString());
                const txt = await r.text();
                kvOut.textContent = txt;
                addHistory('GET', `key=${key()} -> ${txt}`, r.ok);
                refreshStats();
            } catch (e) {
                kvOut.textContent = String(e);
                addHistory('GET', String(e), false);
            }
        }

        async function delValue() {
            try {
                const q = new URLSearchParams({ key: key() });
                const r = await fetch('/del?' + q.toString(), { method: 'DELETE' });
                const txt = await r.text();
                kvOut.textContent = txt;
                addHistory('DEL', `key=${key()} -> ${txt}`, r.ok);
                refreshStats();
            } catch (e) {
                kvOut.textContent = String(e);
                addHistory('DEL', String(e), false);
            }
        }

        async function runCmd() {
            try {
                const body = document.getElementById('cmd').value;
                const r = await fetch('/cmd', { method: 'POST', body });
                const txt = await r.text();
                cmdOut.textContent = txt;
                addHistory('CMD', `${body} -> ${txt}`, r.ok);
                refreshStats();
            } catch (e) {
                cmdOut.textContent = String(e);
                addHistory('CMD', String(e), false);
            }
        }

        async function ping() {
            try {
                const r = await fetch('/health');
                const txt = await r.text();
                cmdOut.textContent = txt;
                addHistory('PING', txt, r.ok);
            } catch (e) {
                cmdOut.textContent = String(e);
                addHistory('PING', String(e), false);
            }
        }

        async function refreshStats() {
            try {
                const r = await fetch('/stats');
                const txt = await r.text();
                const s = JSON.parse(txt);

                document.getElementById('s_hits').textContent = fmtNum(s.hits);
                document.getElementById('s_misses').textContent = fmtNum(s.misses);
                document.getElementById('s_ratio').textContent = Number(s.hit_ratio || 0).toFixed(4);
                document.getElementById('s_active').textContent = fmtNum(s.active_keys);
                document.getElementById('s_mem').textContent = fmtNum(s.memory_bytes);
                document.getElementById('s_evict').textContent = fmtNum(s.evictions);
                document.getElementById('s_exp').textContent = fmtNum(s.expirations);
                document.getElementById('s_status').textContent = 'UP';

                document.getElementById('statsRaw').textContent = txt;
                healthBadge.textContent = 'Server: Healthy';
            } catch (e) {
                document.getElementById('s_status').textContent = 'DOWN';
                document.getElementById('statsRaw').textContent = String(e);
                healthBadge.textContent = 'Server: Unreachable';
            }
        }

        window.addEventListener('keydown', (e) => {
            if (e.key === 'Enter' && e.ctrlKey) {
                runCmd();
            }
        });

        refreshStats();
        setInterval(refreshStats, 2000);
    </script>
</body>
</html>
        )HTML";
}

}  // namespace

int main(int argc, char** argv) {
    int port = 8080;
    std::size_t capacity = 50000;

    if (argc > 1) {
        port = std::atoi(argv[1]);
    }
    if (argc > 2) {
        try {
            capacity = static_cast<std::size_t>(std::stoull(argv[2]));
        } catch (...) {
            std::cerr << "Invalid capacity. Usage: cache_http [port] [capacity]\n";
            return 1;
        }
    }

    mini_redis::RedisCommandProcessor processor(capacity);
    httplib::Server server;

    server.Get("/", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(ui_html(), "text/html; charset=utf-8");
    });

    server.Get("/health", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("{\"ok\":true,\"status\":\"up\"}", "application/json");
    });

    server.Post("/set", [&](const httplib::Request& req, httplib::Response& res) {
        try {
            if (!req.has_param("key") || !req.has_param("value")) {
                res.status = 400;
                res.set_content(json_err("usage: POST /set?key=<k>&value=<v>[&ttl_ms=<n>]]"), "application/json");
                return;
            }

            const auto ttl = parse_ttl_ms(req);
            const bool ok = processor.set(req.get_param_value("key"), req.get_param_value("value"), ttl);
            if (!ok) {
                res.status = 500;
                res.set_content(json_err("set failed"), "application/json");
                return;
            }

            res.set_content(json_ok("OK"), "application/json");
        } catch (const std::exception& ex) {
            res.status = 400;
            res.set_content(json_err(ex.what()), "application/json");
        }
    });

    server.Get("/get", [&](const httplib::Request& req, httplib::Response& res) {
        if (!req.has_param("key")) {
            res.status = 400;
            res.set_content(json_err("usage: GET /get?key=<k>"), "application/json");
            return;
        }

        const auto value = processor.get(req.get_param_value("key"));
        if (!value.has_value()) {
            res.status = 404;
            res.set_content("{\"ok\":true,\"value\":null}", "application/json");
            return;
        }

        res.set_content(std::string{"{\"ok\":true,\"value\":\""} + json_escape(*value) + "\"}", "application/json");
    });

    server.Delete("/del", [&](const httplib::Request& req, httplib::Response& res) {
        if (!req.has_param("key")) {
            res.status = 400;
            res.set_content(json_err("usage: DELETE /del?key=<k>"), "application/json");
            return;
        }

        const bool deleted = processor.del(req.get_param_value("key"));
        res.set_content(std::string{"{\"ok\":true,\"deleted\":"} + (deleted ? "true}" : "false}"), "application/json");
    });

    server.Get("/stats", [&](const httplib::Request&, httplib::Response& res) {
        const auto s = processor.stats();
        std::string payload =
            "{\"ok\":true"
            ",\"hits\":" + std::to_string(s.hits) +
            ",\"misses\":" + std::to_string(s.misses) +
            ",\"hit_ratio\":" + std::to_string(s.hit_ratio()) +
            ",\"evictions\":" + std::to_string(s.evictions) +
            ",\"expirations\":" + std::to_string(s.expirations) +
            ",\"active_keys\":" + std::to_string(s.active_keys) +
            ",\"memory_bytes\":" + std::to_string(s.memory_bytes) +
            "}";
        res.set_content(payload, "application/json");
    });

    server.Post("/cmd", [&](const httplib::Request& req, httplib::Response& res) {
        const auto response = processor.execute_line(req.body);
        if (!response.ok) {
            res.status = 400;
            res.set_content(json_err(response.payload), "application/json");
            return;
        }
        res.set_content(json_ok(response.payload), "application/json");
    });

    std::cout << "Mini Redis HTTP server listening on port " << port << "\n";
    std::cout << "Endpoints: GET /health, POST /set, GET /get, DELETE /del, GET /stats, POST /cmd\n";

    if (!server.listen("0.0.0.0", port)) {
        std::cerr << "Failed to start server\n";
        return 1;
    }

    return 0;
}
