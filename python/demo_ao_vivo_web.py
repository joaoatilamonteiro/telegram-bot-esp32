"""
Demo Web de deteccao de MOVIMENTO por CSI (FastAPI + Telegram Cooldown).
Uso:
  python demo_ao_vivo_web.py --ip <IP_DA_ESP> --serial <PORTA> --token <TOKEN_BOT> --chat-id <SEU_CHAT_ID>
"""

import argparse
import csv
import datetime
import socket
import threading
import time
from collections import deque
import requests
import uvicorn
from fastapi import FastAPI
from fastapi.responses import HTMLResponse
import numpy as np

import csi_features as cf

TAMANHO = 384
N_SUB = cf.N_SUB
MIN_PKTS = cf.MIN_PKTS
JANELA_MS = cf.JANELA_MS
PASSO_MS = cf.PASSO_MS

app = FastAPI()

# ----------------------------------------------------------------------------
# HTML do Painel (Front-end)
# ----------------------------------------------------------------------------
PAGINA_HTML = """
<!DOCTYPE html>
<html>
<head>
    <title>Radar CSI Ao Vivo</title>
    <script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
    <style>
        body { font-family: sans-serif; text-align: center; background-color: #f4f4f9; }
        #statusBox { padding: 20px; margin: 20px auto; width: 60%; border-radius: 10px; font-size: 24px; font-weight: bold; }
        .calibrando { background-color: #f1c40f; color: #333; }
        .parado { background-color: #2ecc71; color: white; }
        .movimento { background-color: #e74c3c; color: white; }
        .info { font-size: 14px; color: #555; margin-top: 10px; }
        canvas { max-width: 1000px; height: 450px; margin: 0 auto; background: white; border-radius: 8px; box-shadow: 0 4px 6px rgba(0,0,0,0.1); }
    </style>
</head>
<body>
    <h1>Radar CSI - Painel de Controle</h1>
    <div id="statusBox" class="calibrando">Carregando...</div>
    <div class="info" id="infoTexto">Aguardando dados da porta serial...</div>
    <div class="info" id="armadoTexto" style="font-weight:bold; color:blue;">Estado: DESARMADO</div>
    <canvas id="graficoCSI"></canvas>
    
    <script>
        const ctx = document.getElementById('graficoCSI').getContext('2d');
        const chart = new Chart(ctx, {
            type: 'line',
            data: { labels: [], datasets: [{ label: 'Oscilação', data: [], borderColor: '#3498db', borderWidth: 2, fill: false },
                                           { label: 'Limiar', data: [], borderColor: '#e74c3c', borderWidth: 2, borderDash: [5, 5], fill: false }] },
            options: { responsive: true, animation: false, scales: { x: { title: { display: true, text: 'Segundos atrás' } }, y: { beginAtZero: true } } }
        });

        async function atualizar() {
            try {
                const res = await fetch('/status');
                const data = await res.json();
                
                const box = document.getElementById('statusBox');
                if (data.fase === 'calibrando') {
                    box.className = 'calibrando';
                    box.innerText = `Calibrando... ${Math.round(data.prog * 100)}%`; 
                } else { 
                    box.className = data.alerta ? 'movimento' : 'parado'; 
                    box.innerText = data.alerta ? 'MOVIMENTO DETECTADO' : 'SEM MOVIMENTO'; 
                }  
                
                document.getElementById('infoTexto').innerText = `Pacotes/s: ${data.pps} | Valor: ${data.valor ? data.valor.toFixed(2) : '--'} | Limiar: ${data.limiar ? data.limiar.toFixed(2) : '--'}`;
                document.getElementById('armadoTexto').innerText = data.armado ? 'Estado: 🛡️ ARMADO' : 'Estado: 🛌 DESARMADO';

                if (data.hist.length > 0) {
                    const labels = data.hist.map((_, i) => -(data.hist.length - 1 - i) * 0.5);
                    chart.data.labels = labels;
                    chart.data.datasets[0].data = data.hist.reverse(); // Ajuste de ordem
                    chart.data.datasets[1].data = data.hist.map(() => data.limiar);
                    chart.update();
                }
            } catch (err) { console.error(err); }
        }
        setInterval(atualizar, 500);
    </script>
</body>
</html>
"""

def parse_linha(linha):
    p = linha.split(",")
    if len(p) != 3 + TAMANHO: return None
    try:
        ts = int(p[0])
        if int(p[2]) != TAMANHO: return None
        v = np.array(p[3:], dtype=np.float32)
    except ValueError: return None
    return ts, np.hypot(v[0::2], v[1::2])[:N_SUB]

class Detector:
    def __init__(self, calib_s=60.0, sensib=1.0, k=3.0, n=10, m=4, modelo=None):
        self.calib_s, self.sensib, self.k, self.n, self.m = calib_s, sensib, k, n, m
        self.modelo = modelo
        self.lock = threading.Lock()
        self.armado = False  # Estado do alarme controlado via Telegram
        self.reset()

    def reset(self):
        self.fase = "calibrando"
        self.calib = []
        self.base = None
        self.med = self.sigma = None
        self.janela = deque()
        self.ultimo_calc = None
        self.ultimo_ts = None
        self.flags = deque(maxlen=self.n)
        self.historico = deque(maxlen=60) # Guarda 30 segundos no grafico
        self.valor = None
        self.alerta = False
        self.pps = 0
        self.prog = 0.0
        self.msg = ""
        self.total = 0

    @property
    def limiar(self):
        if self.modelo is not None:
            return float(np.clip(0.5 / self.sensib, 0.05, 0.95))
        return self.med + (self.k / self.sensib) * self.sigma if self.med else 0.0

    def _finalizar_calibracao(self):
        ts = np.array([t for t, _ in self.calib], dtype=np.float64)
        arr = np.stack([a for _, a in self.calib])
        pps = len(ts) / max((ts[-1] - ts[0]) / 1000.0, 1e-9)
        base, F = cf.calibrar(ts, arr)
        if base is None:
            self.calib = []
            return
        self.base = base
        z = F[:, 0]
        self.med = float(np.median(z))
        self.sigma = max(1.4826 * float(np.median(np.abs(z - self.med))), 0.05 * self.med)
        self.fase = "ao_vivo"

    def add(self, ts, amp):
        with self.lock:
            self.total += 1
            if self.ultimo_ts is not None and ts < self.ultimo_ts:
                self.janela.clear()
                self.ultimo_calc = None
                self.calib = []
            self.ultimo_ts = ts

            if self.fase == "calibrando":
                self.calib.append((ts, amp))
                dec = (ts - self.calib[0][0]) / 1000.0
                self.prog = min(dec / self.calib_s, 1.0)
                if dec >= self.calib_s:
                    self._finalizar_calibracao()
                return False

            self.janela.append((ts, amp[self.base.mask]))
            while self.janela and self.janela[0][0] <= ts - JANELA_MS:
                self.janela.popleft()
            self.pps = len(self.janela)

            if self.ultimo_calc is None or ts - self.ultimo_calc >= PASSO_MS:
                self.ultimo_calc = ts
                if len(self.janela) >= MIN_PKTS:
                    raw = np.stack([r for _, r in self.janela])
                    x = raw / np.clip(raw.mean(axis=1, keepdims=True), 1e-9, None)
                    f = cf.features_janela(self.base, x, raw)
                    self.valor = float(self.modelo.predict_proba(f[None, :])[0, 1]) if self.modelo else float(f[0])
                    self.flags.append(self.valor > self.limiar)
                    self.alerta = sum(self.flags) >= self.m
                    self.historico.append(self.valor)
                    return True
                self.valor = None
            return False

    def snapshot(self):
        with self.lock:
            return dict(fase=self.fase, prog=self.prog, valor=self.valor, alerta=self.alerta,
                        pps=self.pps, msg=self.msg, armado=self.armado,
                        limiar=(self.limiar if self.fase == "ao_vivo" else None),
                        hist=list(self.historico))

def disparar_udp(ip, porta, intervalo_s):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    prox = time.perf_counter()
    while True:
        try: sock.sendto(b"radar", (ip, porta))
        except OSError: pass
        prox += intervalo_s
        atraso = prox - time.perf_counter()
        if atraso > 0: time.sleep(atraso)
        else: prox = time.perf_counter()

# Instância global do detector para ser lida pelas rotas do FastAPI
det = Detector(calib_s=60.0)

# Dicionário global para controle de estado do alarme e cooldown
estado_alerta = {
    "ultimo_envio": 0.0,
    "eventos_durante_cooldown": 0
}

def achar_porta():
    import serial.tools.list_ports
    portas = list(serial.tools.list_ports.comports())
    cand = [p for p in portas if any(c in (p.description or "").upper() for c in ("CP210", "CH340", "USB", "UART"))]
    if len(cand) == 1: return cand[0].device
    raise SystemExit("Porta não detectada automaticamente. Informe --serial COMx.")

def enviar_alerta_telegram(token, chat_id, link, cooldown_segundos):
    agora = datetime.datetime.now()
    proximo_aviso = agora + datetime.timedelta(seconds=cooldown_segundos)
    hora_proxima = proximo_aviso.strftime("%H:%M:%S")
    
    extras = estado_alerta["eventos_durante_cooldown"]
    
    url = f"https://api.telegram.org/bot{token}/sendMessage"
    
    msg = (
        f"🚨 *ALERTA DE MOVIMENTO DETECTADO!* 🚨\n\n"
        f"O radar interceptou atividade na área.\n"
    )
    
    if extras > 0:
        msg += f"📊 *Atividade no cooldown:* {extras} novo(s) pico(s) registrado(s).\n"
        
    msg += (
        f"⏳ *Próximo alerta liberado após:* {hora_proxima}\n\n"
        f"Acompanhe os gráficos ao vivo no painel:\n{link}"
    )
    
    try:
        requests.post(url, json={"chat_id": chat_id, "text": msg, "parse_mode": "Markdown"}, timeout=5)
        # Zera o contador após enviar o relatório
        estado_alerta["eventos_durante_cooldown"] = 0
    except Exception as e:
        print(f"Erro ao enviar Telegram: {e}")

@app.get("/")
def ler_painel():
    return HTMLResponse(content=PAGINA_HTML, status_code=200)

@app.get("/status")
def ler_status():
    return det.snapshot()

def main():
    p = argparse.ArgumentParser()
    p.add_argument("--serial", default=None)
    p.add_argument("--ip", required=True)
    p.add_argument("--token", required=True, help="Token do Bot do Telegram")
    p.add_argument("--chat-id", required=True, help="Seu Chat ID do Telegram")
    p.add_argument("--link", default="http://192.168.1.165:8000", help="Link que vai na mensagem")
    p.add_argument("--cooldown", type=int, default=300, help="Segundos de silêncio do Telegram após alerta (padrão: 5 min)")
    p.add_argument("--modelo", default=None, help="Caminho para o arquivo da árvore treinada (ex: modelo_arvore.joblib)")
    args = p.parse_args()

    modelo = None
    if args.modelo:
        import joblib
        pacote = joblib.joblib.load(args.modelo) if hasattr(joblib, 'joblib') else joblib.load(args.modelo)
        modelo = pacote["modelo"]
        print(f"[modelo] Árvore de IA carregada com sucesso de {args.modelo}")

    # Passa o modelo para o detector global
    global det
    det = Detector(calib_s=60.0, modelo=modelo)

    porta = args.serial or achar_porta()
    print(f"Iniciando captura na {porta} e UDP para {args.ip}...")

    # Thread 1: Disparo UDP Constante
    threading.Thread(target=disparar_udp, args=(args.ip, 5555, 20.0 / 1000.0), daemon=True).start()

    # Função de processamento embutida
    def loop_serial():
        import serial
        ser = serial.Serial(porta, 921600, timeout=0.1)
        buf = bytearray()
        while True:
            dados = ser.read(max(ser.in_waiting, 1))
            if not dados: continue
            buf += dados
            while True:
                i = buf.find(b"\n")
                if i < 0: break
                linha = bytes(buf[:i]).decode("utf-8", errors="replace").strip()
                del buf[:i + 1]
                
                # INTERCEPTAÇÃO DO COMANDO DO TELEGRAM VIA ESP32
                if "[CMD:ALARME_ON]" in linha:
                    det.armado = True
                    print("--> Radar ARMADO pelo comando do Telegram!")
                    continue
                elif "[CMD:ALARME_OFF]" in linha:
                    det.armado = False
                    print("--> Radar DESARMADO pelo comando do Telegram!")
                    continue

                pkt = parse_linha(linha)
                if pkt:
                    novo = det.add(*pkt)
                    if novo:
                        # Logica de gatilho do Telegram
                        if det.alerta and det.armado:
                            agora_ts = time.time()
                            tempo_decorrido = agora_ts - estado_alerta["ultimo_envio"]
                            
                            # Só notifica se passou o tempo de cooldown
                            if tempo_decorrido > args.cooldown:
                                estado_alerta["ultimo_envio"] = agora_ts
                                print("Disparando notificação no Telegram com o relatório!")
                                # Passa todos os 4 argumentos exigidos pela função (incluindo args.cooldown)
                                threading.Thread(
                                    target=enviar_alerta_telegram, 
                                    args=(args.token, args.chat_id, args.link, args.cooldown), 
                                    daemon=True
                                ).start()
                            else:
                                # Se ainda estamos no cooldown, apenas soma +1 no contador silencioso
                                estado_alerta["eventos_durante_cooldown"] += 1
            
            if len(buf) > 50000: buf.clear()

    # Thread 2: Escuta da Porta Serial
    threading.Thread(target=loop_serial, daemon=True).start()

    # Thread de Diagnóstico de Pacotes
    def monitor_diagnostico():
        with open("log_radar.txt", "a", encoding="utf-8") as f:
            f.write(f"\n--- Iniciando Diagnóstico: {datetime.datetime.now().strftime('%d/%m %H:%M:%S')} ---\n")
            while True:
                time.sleep(2)  # Analisa a cada 2 segundos
                pps = det.pps
                # Se estiver no modo ao vivo e os pacotes caírem para menos de 30 na janela, registra o erro
                if det.fase == "ao_vivo" and pps < 30:
                    aviso = f"[{datetime.datetime.now().strftime('%H:%M:%S')}] ALERTA DE QUEDA: Apenas {pps} pacotes detectados na janela atual.\n"
                    print(aviso.strip())
                    f.write(aviso)
                    f.flush()


    print("Servidor Web iniciando. Acesse a sua rede local pelo navegador (ex: http://192.168.1.165:8000) para ver o painel.")
    # Inicia o FastAPI corretamente liberado para acesso na rede local
    uvicorn.run(app, host="0.0.0.0", port=8000, log_level="warning")


    threading.Thread(target=monitor_diagnostico, daemon=True).start()
if __name__ == "__main__":
    main()