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
import traceback
import time
from collections import deque
import requests
import uvicorn
from fastapi import FastAPI
from fastapi.responses import HTMLResponse
import numpy as np
import re
from pathlib import Path
import csi_features as cf
import joblib
import serial
import serial.tools.list_ports

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
                    chart.data.datasets[0].data = data.hist; // historico ja vem do mais antigo para o mais novo
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
        self.rec_ts = deque()      # timestamps do ultimo 1 s (pps tambem durante a calibracao)
        self.falhas_calib = 0

    @property
    def limiar(self):
        if self.modelo is not None:
            return float(np.clip(0.5 / self.sensib, 0.05, 0.95))
        return self.med + (self.k / self.sensib) * self.sigma if self.med is not None else 0.0

    def _finalizar_calibracao(self):
        ts = np.array([t for t, _ in self.calib], dtype=np.float64)
        arr = np.stack([a for _, a in self.calib])
        pps = len(ts) / max((ts[-1] - ts[0]) / 1000.0, 1e-9)
        base, F = cf.calibrar(ts, arr)
        if base is None:
            self.falhas_calib += 1
            print(f"[calib] FALHOU (#{self.falhas_calib}): media de {pps:.1f} pacotes/s; "
                  f"sao necessarios >= {MIN_PKTS} pacotes por janela de 1 s. Reiniciando a calibracao. "
                  f"Verifique se o PC esta disparando UDP para a ESP e se a rede nao isola os clientes.")
            self.calib = []
            self.rec_ts.clear()
            self.prog = 0.0
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
                print("[serial] timestamp voltou no tempo: a ESP reiniciou. Calibracao/janela reiniciadas.")
                self.janela.clear()
                self.rec_ts.clear()
                self.ultimo_calc = None
                self.calib = []
            self.ultimo_ts = ts

            if self.fase == "calibrando":
                self.rec_ts.append(ts)
                while self.rec_ts and self.rec_ts[0] <= ts - JANELA_MS:
                    self.rec_ts.popleft()
                self.pps = len(self.rec_ts)
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

def disparar_udp(alvo, porta, intervalo_s):
    """Envia UDP continuo para alvo['ip'] (o dict permite corrigir o IP em tempo de execucao)."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    prox = time.perf_counter()
    while True:
        ip = alvo["ip"]
        if ip:
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


def enviar_status_radar(token, chat_id, link):
    s = det.snapshot()

    if s["fase"] == "calibrando":
        situacao = f"⏳ Calibrando... {int(s['prog'] * 100)}% (mantenha o ambiente parado)"
    elif s["alerta"]:
        situacao = "🔴 Movimento detectado agora"
    else:
        situacao = "🟢 Sem movimento"

    armado = "🛡️ ARMADO" if s["armado"] else "🛌 DESARMADO"
    ultimo = estado_alerta["ultimo_envio"]
    ultimo_txt = datetime.datetime.fromtimestamp(ultimo).strftime("%H:%M:%S") if ultimo else "nenhum"

    msg = (f"📡 Radar\n"
           f"Estado: {armado}\n"
           f"Situação: {situacao}\n"
           f"Pacotes/s: {s['pps']}\n"
           f"Último alerta enviado: {ultimo_txt}\n"
           f"Painel: {link}")
    if s["fase"] == "ao_vivo" and s["pps"] < 30:
        msg += "\n⚠️ Poucos pacotes por segundo: a leitura pode estar pouco confiável."

    try:
        requests.post(f"https://api.telegram.org/bot{token}/sendMessage",
                      json={"chat_id": chat_id, "text": msg}, timeout=5)
    except Exception as e:
        print(f"Erro ao enviar status do radar: {e}")


@app.get("/")
def ler_painel():
    return HTMLResponse(content=PAGINA_HTML, status_code=200)

@app.get("/status")
def ler_status():
    return det.snapshot()

SECRETS_H = Path(__file__).resolve().parent.parent / "src" / "secrets.h"

def ler_secret(nome):
    try:
        texto = SECRETS_H.read_text(encoding="utf-8", errors="ignore")
    except OSError:
        return None
    m = re.search(rf'\b{nome}\s*=\s*"([^"]*)"', texto)
    return m.group(1) if m else None

def ip_local():
    """IP do PC na rede (nao envia nada, so descobre qual interface seria usada)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("10.255.255.255", 1))
        return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        s.close()


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--serial", default=None)
    p.add_argument("--ip", default=None, help="IP do ESP32 (se omitido, e lido da serial; o IP da serial sempre prevalece)")
    p.add_argument("--token", default=None, help="Token do Bot do Telegram")
    p.add_argument("--chat-id", default=None, help="Seu Chat ID do Telegram")
    p.add_argument("--link", default=None, help="Link que vai na mensagem (padrao: IP do PC na porta 8000)")
    p.add_argument("--cooldown", type=int, default=300, help="Segundos de silencio do Telegram apos alerta (padrao: 5 min)")
    p.add_argument("--modelo", default=None, help="Caminho para o arquivo da arvore treinada (ex: modelo_arvore.joblib)")
    args = p.parse_args()
    args.token = args.token or ler_secret("BOT_TOKEN")
    args.chat_id = args.chat_id or ler_secret("chatID")
    if not args.token or not args.chat_id:
        raise SystemExit("Token/chat ID nao informados e nao encontrados em src/secrets.h. Use --token e --chat-id.")
    args.link = args.link or f"http://{ip_local()}:8000"

    modelo = None
    if args.modelo:
        pacote = joblib.load(args.modelo)
        modelo = pacote["modelo"]
        print(f"[modelo] Arvore de IA carregada com sucesso de {args.modelo}")

    # Passa o modelo para o detector global
    global det
    det = Detector(calib_s=60.0, modelo=modelo)

    porta = args.serial or achar_porta()
    print(f"Iniciando captura na {porta} e UDP para {args.ip or '(IP lido da serial)'}...")

    # ------------------------------------------------------------------
    # Thread 1: disparo UDP constante. O alvo e um dict para poder ser
    # corrigido quando a ESP informar o IP real pela serial.
    # ------------------------------------------------------------------
    alvo = {"ip": None}
    udp_thread = {"on": False}

    def iniciar_disparo(ip):
        if alvo["ip"] == ip:
            return
        antigo = alvo["ip"]
        alvo["ip"] = ip
        if antigo:
            print(f"[udp] alvo mudou de {antigo} para {ip}")
        else:
            print(f"[udp] disparando para {ip}")
        if not udp_thread["on"]:
            udp_thread["on"] = True
            threading.Thread(target=disparar_udp, args=(alvo, 5555, 20.0 / 1000.0), daemon=True).start()

    if args.ip:
        iniciar_disparo(args.ip)

    # ------------------------------------------------------------------
    # Processamento de uma linha vinda da serial
    # ------------------------------------------------------------------
    def processar_linha(linha):
        if "Endereço IP:" in linha:
            m = re.search(r"\d+\.\d+\.\d+\.\d+", linha)
            if m:
                ip_real = m.group()
                if args.ip and ip_real != args.ip:
                    print(f"[udp] AVISO: --ip {args.ip} difere do IP informado pela ESP ({ip_real}). Usando {ip_real}.")
                iniciar_disparo(ip_real)
            return

        # Comandos do Telegram que a ESP repassa pela serial
        if "[CMD:ALARME_ON]" in linha:
            det.armado = True
            print("--> Radar ARMADO pelo comando do Telegram!")
            return
        if "[CMD:ALARME_OFF]" in linha:
            det.armado = False
            print("--> Radar DESARMADO pelo comando do Telegram!")
            return
        if "[CMD:STATUS_RADAR]" in linha:
            threading.Thread(target=enviar_status_radar,
                             args=(args.token, args.chat_id, args.link), daemon=True).start()
            return

        pkt = parse_linha(linha)
        if not pkt:
            return
        novo = det.add(*pkt)
        if novo and det.alerta and det.armado:
            agora_ts = time.time()
            tempo_decorrido = agora_ts - estado_alerta["ultimo_envio"]
            # So notifica se passou o tempo de cooldown
            if tempo_decorrido > args.cooldown:
                estado_alerta["ultimo_envio"] = agora_ts
                print("Disparando notificacao no Telegram com o relatorio!")
                threading.Thread(
                    target=enviar_alerta_telegram,
                    args=(args.token, args.chat_id, args.link, args.cooldown),
                    daemon=True,
                ).start()
            else:
                # Ainda no cooldown: so soma +1 no contador silencioso
                estado_alerta["eventos_durante_cooldown"] += 1

    # ------------------------------------------------------------------
    # Thread 2: leitura da serial. Nunca morre em silencio: erros de porta
    # (ocupada, cabo solto) fazem reconectar; erro em uma linha so a descarta.
    # ------------------------------------------------------------------
    def loop_serial():
        while True:
            try:
                ser = serial.Serial(porta, 921600, timeout=0.1)
            except (serial.SerialException, OSError) as e:
                print(f"[serial] nao consegui abrir {porta}: {e}")
                print("[serial] Feche monitores seriais (PlatformIO/Arduino), confira --serial e o cabo. Tentando de novo em 3 s...")
                time.sleep(3)
                continue

            print(f"[serial] {porta} aberta a 921600 baud")
            buf = bytearray()
            try:
                while True:
                    dados = ser.read(max(ser.in_waiting, 1))
                    if not dados:
                        continue
                    buf += dados
                    while True:
                        i = buf.find(b"\n")
                        if i < 0:
                            break
                        linha = bytes(buf[:i]).decode("utf-8", errors="replace").strip()
                        del buf[:i + 1]
                        try:
                            processar_linha(linha)
                        except Exception:
                            print("[serial] erro ao processar uma linha (descartada):")
                            traceback.print_exc()
                    if len(buf) > 50000:
                        buf.clear()
            except (serial.SerialException, OSError) as e:
                print(f"[serial] conexao perdida: {e}. Reconectando em 3 s...")
                try:
                    ser.close()
                except Exception:
                    pass
                time.sleep(3)

    threading.Thread(target=loop_serial, daemon=True).start()

    # ------------------------------------------------------------------
    # Thread 3: diagnostico. Escreve em TODAS as fases (inclusive calibrando),
    # em log_radar.txt ao lado deste script, linha a linha.
    # ------------------------------------------------------------------
    def monitor_diagnostico():
        caminho = Path(__file__).resolve().with_name("log_radar.txt")
        print(f"[diag] gravando log em {caminho}")
        ultimo_total = 0
        ticks = 0
        with open(caminho, "a", encoding="utf-8", buffering=1) as f:
            f.write(f"\n--- Diagnostico: {datetime.datetime.now():%d/%m %H:%M:%S} ---\n")
            while True:
                time.sleep(2)
                ticks += 1
                total = det.total
                pps_real = (total - ultimo_total) / 2.0
                ultimo_total = total

                dica = ""
                if alvo["ip"] is None:
                    dica = "UDP NAO iniciado: IP da ESP desconhecido (use --ip ou regrave a ESP para imprimir o IP periodicamente)"
                elif total == 0:
                    dica = "nenhuma linha CSI valida chegou da serial (porta/baud/firmware?)"
                elif pps_real < 20:
                    dica = "trafego insuficiente (<20 pacotes/s): confira ping PC->ESP, isolamento de clientes no AP e o IP alvo"
                elif pps_real < 30:
                    dica = "trafego baixo (<30 pacotes/s)"

                linha = (f"[{datetime.datetime.now():%H:%M:%S}] fase={det.fase} prog={det.prog:.0%} "
                         f"pacotes/s={pps_real:.0f} alvo_udp={alvo['ip']} calib_falhas={det.falhas_calib}")
                if dica:
                    linha += f" <-- {dica}"
                f.write(linha + "\n")
                # Terminal: a cada 10 s, ou a cada 2 s se houver problema
                if dica or ticks % 5 == 0:
                    print(linha)

    threading.Thread(target=monitor_diagnostico, daemon=True).start()

    print(f"Servidor Web iniciando. Acesse {args.link} (ou http://localhost:8000) para ver o painel.")
    uvicorn.run(app, host="0.0.0.0", port=8000, log_level="warning")


if __name__ == "__main__":
    main()