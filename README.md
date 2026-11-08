
# Telegram Bot ESP32 + Radar Wi-Fi (CSI)

Este projeto transforma um ESP32 em um assistente residencial versátil controlado via Telegram. Ele atua como uma ponte para controlar o seu computador pela rede local e também funciona simultaneamente como um **radar de movimento invisível**, utilizando a distorção do sinal Wi-Fi (CSI) para detectar presença no ambiente e enviar alertas.

## Funcionalidades

**Controle do Computador (PC)**
* `ligar computador`: Envia um pacote mágico (Wake-on-LAN) para ligar o PC remotamente.
* `desligar computador`: Localiza o PC na rede automaticamente via broadcast UDP, valida o MAC Address e envia o comando fatal para o agente `escuta.py`.
* `status`: Verifica se o PC está online na rede respondendo na porta 5000.

**Radar CSI (Detecção de Movimento Wi-Fi)**
* `ativar radar` e `desativar radar`: Armam/desarmam o sistema de segurança via Telegram.
* **Sem câmeras:** Usa a variação do sinal Wi-Fi (Channel State Information) coletada no núcleo 0 do ESP32 para detectar movimento.
* **Painel Web Ao Vivo:** Interface local em Python (FastAPI) rodando no PC para visualizar os gráficos do radar, limiares e pacotes por segundo em tempo real.
* **Alertas Inteligentes:** Notificações instantâneas no Telegram com sistema de *cooldown* (padrão de 5 min) e relatório de picos no período.

---

## Como Configurar e Instalar

### 1. Configuração do ESP32 (Firmware)
Crie um arquivo `src/secrets.h` com as suas credenciais:
```cpp
#define WIFI_SSID "SuaRedeWiFi"
#define WIFI_PASSWORD "SuaSenha"
#define BOT_TOKEN "TokenDoSeuBot"
#define chatID "SeuChatID"
#define MAC_PC "AA:BB:CC:DD:EE:FF"


Compile e grave o firmware na placa utilizando o **PlatformIO** (`pio run -t upload`).
*Nota: A comunicação serial agora roda a **921600 baud**.*

### 2. Configuração no PC (Windows)

O projeto exige que alguns scripts rodem no computador para receber os comandos e processar os dados do radar.

**Para o controle do PC (escuta.py):**
Instale a dependência de rede e inicie o agente (lembre-se de liberar a porta UDP/TCP 5000 no firewall):

```powershell
pip install getmac
python src/escuta.py
```

**Para o Painel do Radar CSI:**
O radar exige que o PC esteja ligado à ESP32 via USB para processar as ondas e hospedar o servidor web.
Instale as dependências:

```powershell
pip install numpy pyserial requests fastapi uvicorn joblib scikit-learn
```

Inicie o servidor (ajuste as portas e IPs conforme sua rede):

```powershell
python python/demo_ao_vivo_web.py --ip <IP_DO_ESP32> --serial <SUA_PORTA_COM> --token <TOKEN_DO_BOT> --chat-id <SEU_CHAT_ID> --link http://<IP_DO_PC>:8000 --modelo python/modelo_arvore.joblib
```

Acesse o painel pelo navegador do PC ou celular na mesma rede: `http://<IP_DO_PC>:8000`.

---

## Limitações Conhecidas

* O agente de controle (`escuta.py`) foi desenvolvido apenas para ambiente Windows.
* O sistema CSI detecta **movimento**, não presença estática (pessoas imóveis geram pouca alteração no sinal).
* O painel web roda na rede local sem autenticação.
* Se o script Python do radar reiniciar no PC, o alarme volta ao estado padrão (desarmado).