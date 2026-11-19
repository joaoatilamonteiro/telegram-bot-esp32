#include <Arduino.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <math.h>
#include "painel_web.h"
#include "radar_api.h"
#include "radar_decisao.h"

// Servidor sincrono (WebServer, ja vem no framework) rodando numa task propria:
// continua respondendo mesmo quando o loop() esta bloqueado no Telegram.
static WebServer server(80);

static const char PAGINA[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="pt-BR">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Radar CSI Ao Vivo</title>
<style>
body{font-family:sans-serif;text-align:center;background:#f4f4f9;margin:0;padding:8px}
#statusBox{padding:18px;margin:12px auto;max-width:640px;border-radius:10px;font-size:22px;font-weight:bold}
.calibrando{background:#f1c40f;color:#333}
.parado{background:#2ecc71;color:#fff}
.movimento{background:#e74c3c;color:#fff}
.info{font-size:14px;color:#555;margin:6px}
.cartao{max-width:1000px;margin:14px auto;background:#fff;border-radius:8px;box-shadow:0 4px 6px rgba(0,0,0,.1);padding:8px 12px;text-align:left}
.cartao h2{font-size:15px;margin:4px 0 6px;color:#333}
canvas{width:100%;height:220px;display:block}
.mono{font-family:monospace;font-size:13px;word-break:break-word;color:#333}
</style>
</head>
<body>
<h1>Radar CSI - Painel de Controle</h1>
<div id="statusBox" class="calibrando">Carregando...</div>
<div class="info" id="infoTexto">Aguardando dados...</div>
<div class="info" id="armadoTexto" style="font-weight:bold;color:blue"></div>

<div class="cartao"><h2>Probabilidade de movimento (últimos 30 s)</h2><canvas id="gValor"></canvas></div>
<div class="cartao"><h2>Características usadas pela árvore (últimos 30 s)</h2><canvas id="gFeat"></canvas></div>
<div class="cartao"><h2>Oscilação por subportadora (janela atual)</h2><canvas id="gEsp"></canvas></div>
<div class="cartao"><h2>Decisão da árvore</h2><div id="caminho" class="mono">--</div></div>

<div class="info" id="diag"></div>

<script>
const $ = id => document.getElementById(id);

function prep(cv) {
  const dpr = window.devicePixelRatio || 1;
  const w = cv.clientWidth, h = cv.clientHeight;
  cv.width = Math.round(w * dpr);
  cv.height = Math.round(h * dpr);
  const g = cv.getContext('2d');
  g.setTransform(dpr, 0, 0, dpr, 0, 0);
  g.clearRect(0, 0, w, h);
  return { g: g, w: w, h: h };
}

function grade(g, w, h, ymax, L, R, T, B) {
  g.lineWidth = 1;
  g.strokeStyle = '#e5e5e5';
  g.fillStyle = '#777';
  g.font = '11px sans-serif';
  for (let i = 0; i <= 4; i++) {
    const v = ymax * i / 4;
    const y = T + (h - T - B) * (1 - i / 4);
    g.beginPath(); g.moveTo(L, y); g.lineTo(w - R, y); g.stroke();
    g.fillText(v.toFixed(1), 2, y + 4);
  }
}

function linhaTracejada(g, x1, x2, y, cor) {
  g.strokeStyle = cor; g.lineWidth = 1.5; g.setLineDash([5, 4]);
  g.beginPath(); g.moveTo(x1, y); g.lineTo(x2, y); g.stroke();
  g.setLineDash([]);
}

function linhas(cv, series, refs, ymin) {
  const p = prep(cv), g = p.g, w = p.w, h = p.h;
  const L = 34, R = 8, T = 18, B = 8;
  let ymax = ymin;
  series.forEach(s => s.d.forEach(v => { if (v > ymax) ymax = v; }));
  refs.forEach(r => { if (r.v > ymax) ymax = r.v; });
  ymax *= 1.1;
  grade(g, w, h, ymax, L, R, T, B);
  const Y = v => T + (h - T - B) * (1 - v / ymax);
  refs.forEach(r => linhaTracejada(g, L, w - R, Y(r.v), r.c));
  series.forEach(s => {
    if (s.d.length < 1) return;
    g.strokeStyle = s.c; g.lineWidth = 2; g.beginPath();
    s.d.forEach((v, i) => {
      const x = L + (w - L - R) * (s.d.length > 1 ? i / (s.d.length - 1) : 1);
      if (i === 0) g.moveTo(x, Y(v)); else g.lineTo(x, Y(v));
    });
    g.stroke();
  });
  let x = L + 4;
  g.font = '12px sans-serif';
  series.concat(refs).forEach(s => {
    g.fillStyle = s.c; g.fillText(s.n, x, 12);
    x += g.measureText(s.n).width + 14;
  });
}

function barras(cv, z) {
  const p = prep(cv), g = p.g, w = p.w, h = p.h;
  const L = 34, R = 8, T = 18, B = 16;
  let ymax = 2;
  z.forEach(v => { if (v > ymax) ymax = v; });
  ymax *= 1.1;
  grade(g, w, h, ymax, L, R, T, B);
  const Y = v => T + (h - T - B) * (1 - v / ymax);
  const bw = (w - L - R) / Math.max(z.length, 1);
  z.forEach((v, i) => {
    const x = L + i * bw;
    if (v > 0) {
      g.fillStyle = '#3498db';
      g.fillRect(x + 1, Y(v), Math.max(bw - 2, 1), Y(0) - Y(v));
    } else {
      g.fillStyle = '#ccc';
      g.fillRect(x + 1, Y(0) - 2, Math.max(bw - 2, 1), 2);
    }
  });
  linhaTracejada(g, L, w - R, Y(1), '#2ecc71');
  g.fillStyle = '#777'; g.font = '11px sans-serif';
  g.fillText('subportadora 0', L, h - 3);
  g.fillText('63', w - R - 14, h - 3);
  g.fillStyle = '#2ecc71'; g.font = '12px sans-serif';
  g.fillText('1 = como na calibração (cinza: descartada)', L + 4, 12);
}

function fmtTempo(s) {
  const h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60);
  return h + 'h ' + m + 'min';
}

const f2 = v => (v === null || v === undefined) ? '--' : v.toFixed(2);

async function atualizar() {
  try {
    const res = await fetch('/status', { cache: 'no-store' });
    const d = await res.json();

    const box = $('statusBox');
    if (d.fase === 'calibrando') {
      box.className = 'calibrando';
      box.innerText = 'Calibrando... ' + Math.round(d.prog * 100) + '% (mantenha a área parada)';
    } else {
      box.className = d.alerta ? 'movimento' : 'parado';
      box.innerText = d.alerta ? 'MOVIMENTO DETECTADO' : 'SEM MOVIMENTO';
    }

    $('infoTexto').innerText = 'Pacotes/s: ' + d.pps + ' | Valor: ' + f2(d.valor) +
      ' | Limiar: ' + f2(d.limiar) + ' | osc_p90: ' + f2(d.osc_p90) + ' | energia: ' + f2(d.energia);
    $('armadoTexto').innerText = d.armado ? 'Estado: 🛡️ ARMADO' : 'Estado: 🛌 DESARMADO';

    linhas($('gValor'),
      [{ n: 'Probabilidade', c: '#3498db', d: d.hist }],
      [{ n: 'Limiar', c: '#e74c3c', v: d.limiar }], 1);

    const refsFeat = d.raiz ? [{ n: 'raiz da árvore (' + d.raiz.feat + ')', c: '#e67e22', v: d.raiz.lim }] : [];
    linhas($('gFeat'),
      [{ n: 'osc_p90', c: '#8e44ad', d: d.hist_osc }, { n: 'energia', c: '#16a085', d: d.hist_en }],
      refsFeat, 2);

    barras($('gEsp'), d.z);

    $('caminho').innerText = d.caminho ? d.caminho : '--';
    $('diag').innerText = 'Subportadoras usadas: ' + d.k + ' | Descartes: ' + d.descartes +
      ' | Falhas de calibração: ' + d.falhas + ' | Memória livre: ' + Math.round(d.heap / 1024) +
      ' KB | Ligado há ' + fmtTempo(d.up);
  } catch (err) {
    $('diag').innerText = 'Sem resposta da ESP32...';
  } finally {
    setTimeout(atualizar, 500);
  }
}
atualizar();
</script>
</body>
</html>
)rawliteral";

static float r2(float v) { return roundf(v * 100.0f) / 100.0f; }

static void respondeStatus() {
  EstadoRadar s;
  radarCopiaEstado(s);

  DynamicJsonDocument doc(6144);
  doc["fase"] = (s.fase == RADAR_FASE_AO_VIVO) ? "ao_vivo" : "calibrando";
  doc["prog"] = r2(s.prog);
  if (s.tem_valor) {
    doc["valor"] = r2(s.valor);
    doc["osc_p90"] = r2(s.osc_p90);
    doc["energia"] = r2(s.energia);
  }
  doc["limiar"] = r2(s.limiar);
  doc["alerta"] = s.alerta;
  doc["armado"] = s.armado;
  doc["pps"] = s.pps;
  doc["k"] = s.k;
  doc["falhas"] = s.falhas;
  doc["descartes"] = s.descartes;
  doc["heap"] = ESP.getFreeHeap();
  doc["up"] = (uint32_t)(millis() / 1000);
  doc["caminho"] = s.caminho;

  JsonObject raiz = doc.createNestedObject("raiz");
  raiz["feat"] = radar::nomeFeature(ARV_FEAT[0]);
  raiz["lim"] = r2(ARV_LIM[0]);

  JsonArray h = doc.createNestedArray("hist");
  JsonArray ho = doc.createNestedArray("hist_osc");
  JsonArray he = doc.createNestedArray("hist_en");
  for (int i = 0; i < s.n_hist; i++) {
    h.add(r2(s.hist_valor[i]));
    ho.add(r2(s.hist_osc[i]));
    he.add(r2(s.hist_energia[i]));
  }
  JsonArray z = doc.createNestedArray("z");
  for (int i = 0; i < 64; i++) z.add(r2(s.z[i]));

  String out;
  out.reserve(3072);
  serializeJson(doc, out);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", out);
}

static void tarefaWeb(void *pvParameters) {
  (void)pvParameters;
  for (;;) {
    server.handleClient();
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

void painelSetup() {
  server.on("/", HTTP_GET, []() {
    server.send_P(200, "text/html; charset=utf-8", PAGINA);
  });
  server.on("/status", HTTP_GET, respondeStatus);
  server.onNotFound([]() {
    server.send(404, "text/plain", "nao encontrado");
  });
  server.begin();

  xTaskCreatePinnedToCore(tarefaWeb, "TaskWeb", 8192, NULL, 1, NULL, 1);
}
