# Telegram Bot ESP32 + Radar Wi-Fi (CSI)

Este projeto transforma um ESP32 em um assistente residencial controlado via Telegram. Ele atua como ponte para controlar o seu computador pela rede local e, ao mesmo tempo, funciona como um **radar de movimento invisível**: usa a distorção do sinal Wi-Fi (CSI, Channel State Information) para detectar movimento no ambiente e enviar alertas.

## Funcionalidades

**Controle do computador (PC)**

* `ligar computador`: envia um pacote mágico (Wake-on-LAN) para ligar o PC remotamente.
* `desligar computador`: localiza o PC na rede por broadcast UDP, valida o MAC Address e envia o comando de desligamento ao agente `escuta.py`. O ESP32 tenta até 5 vezes e informa no Telegram se o PC parou de responder.

**Radar CSI (detecção de movimento por Wi-Fi)**

* `ativar radar` e `desativar radar`: armam e desarmam os alertas pelo Telegram.
* `status`: informa se o PC está online (com o IP) e, quando o painel Python está rodando, o estado do radar: armado ou desarmado, situação (calibrando, sem movimento ou movimento), pacotes por segundo e horário do último alerta.
* **Sem câmeras:** o ESP32 mede o CSI de cada pacote Wi-Fi recebido no núcleo 0 e envia os dados pela serial.
* **Painel web ao vivo:** interface local em Python (FastAPI) com a situação atual, o gráfico do valor medido e do limiar e a contagem de pacotes por segundo.
* **Alertas com cooldown:** notificações no Telegram com intervalo de silêncio (padrão de 5 min) e contagem dos picos registrados nesse intervalo.

---

## Estrutura do projeto

```
telegram_bot/
├── platformio.ini              Configuração do PlatformIO
├── README.md
├── src/
│   ├── main.cpp                Firmware: bot do Telegram, Wake-on-LAN e tarefa do radar
│   ├── csi_radar.cpp           Firmware: captura do CSI e tráfego de ping para o roteador
│   └── secrets.h               Credenciais (ignorado pelo git)
└── python/
│   ├── escuta.py               Agente do PC (Windows): responde à busca e executa o desligamento
    ├── demo_ao_vivo_web.py     Painel do radar: serial, detecção, painel web e Telegram
    ├── csi_features.py         Calibração e características do CSI
    ├── modelo_arvore.joblib    Árvore de decisão treinada (movimento / sem movimento)
    ├── requirements.txt        Dependências Python
    ├── log_radar.txt           Log de diagnóstico, gerado ao rodar (ignorado pelo git)
    └── __pycache__/            Cache do Python (ignorado pelo git)
```

---

## Como o radar funciona

1. O PC dispara um pacote UDP para o ESP32 a cada 20 ms (cerca de 50 por segundo) na porta 5555. Esse tráfego existe apenas para gerar pacotes que o ESP32 receba e meça.
2. Para cada pacote recebido, o ESP32 imprime na serial (921600 baud) uma linha CSV com o timestamp, o RSSI, o tamanho e os 384 valores do CSI.
3. O script Python lê a serial e calibra durante **60 segundos**, com o ambiente parado, para aprender como o ambiente fica sem movimento.
4. Depois da calibração, cada janela de 1 segundo (avaliada a cada 0,5 s e com pelo menos 20 pacotes) vira um valor. Com o modelo `.joblib`, esse valor é a probabilidade de movimento e o limiar padrão é 0,5.
5. O alerta é ativado quando pelo menos 4 das últimas 10 janelas passam do limiar. Ele só é enviado ao Telegram se o radar estiver armado e fora do cooldown.

---

## Como configurar e instalar

### 1. Firmware do ESP32

Crie o arquivo `src/secrets.h` com as suas credenciais (ele está no `.gitignore` e não deve ser versionado):

```cpp
#ifndef SECRETS_H
#define SECRETS_H
#include <Arduino.h>

const char* const WIFI_SSID = "SuaRedeWiFi";
const char* const WIFI_PASSWORD = "SuaSenha";

const char* const BOT_TOKEN = "TokenDoSeuBot";

// MAC do PC que será ligado e desligado (6 bytes)
const uint8_t MAC_ALVO[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
const String chatID = "SeuChatID";

#endif
```

Compile e grave o firmware com o **PlatformIO**:

```powershell
pio run -t upload
```

Notas:

* A comunicação serial roda a **921600 baud**.
* O ESP32 só usa a banda de 2.4 GHz. A rede configurada precisa oferecê-la.
* A rede precisa aceitar WPA/WPA2 pessoal. Redes corporativas (WPA2 Enterprise) e com portal de login não são suportadas.
* Para ligar o PC remotamente, o Wake-on-LAN precisa estar habilitado na BIOS e na placa de rede.

### 2. Configuração no PC (Windows)

Crie e ative um ambiente virtual e instale as dependências do painel do radar:

```powershell
python -m venv python\.venv
python\.venv\Scripts\activate
pip install -r python\requirements.txt
```

**Controle do PC (`escuta.py`):** além das dependências acima, o agente usa o `getmac`. Instale e inicie (libere a porta UDP/TCP 5000 no firewall do Windows):

```powershell
pip install getmac
python src/escuta.py
```

**Painel do radar:** o ESP32 precisa estar conectado ao PC por USB.

---

## Usando o radar

1. Alimente o ESP32 por USB e aguarde cerca de 10 segundos para ele conectar ao Wi-Fi.
2. Feche qualquer monitor serial (PlatformIO, Arduino IDE). Só um programa pode usar a porta COM por vez.
3. Na raiz do projeto, inicie o painel. O IP do ESP32 é detectado automaticamente, então não é preciso informá-lo:

   ```powershell
   python python/demo_ao_vivo_web.py --serial COM5 --modelo python/modelo_arvore.joblib
   ```

   O terminal deve mostrar `[serial] COM5 aberta` e `[udp] disparando para <IP>`. Esse é o IP detectado do ESP32.

4. Se a linha `[udp] disparando` não aparecer, o IP não foi detectado. O firmware imprime o IP uma única vez, ao ligar. Reinicie o ESP32 (botão EN) com o script rodando, ou informe o IP manualmente com `--ip <IP_DO_ESP32>`.
5. Confira que o PC alcança o ESP32 com `ping <IP>`, usando o IP mostrado no terminal.
6. Abra `http://localhost:8000` no PC, ou `http://<IP_DO_PC>:8000` no celular na mesma rede.
7. Mantenha o ambiente **parado por 60 segundos** até aparecer "SEM MOVIMENTO".
8. No Telegram, envie `ativar radar` para armar os alertas.

### Argumentos do painel

| Argumento | Descrição |
|---|---|
| `--serial` | Porta COM do ESP32. Se omitido, o script tenta detectar uma única porta USB. |
| `--ip` | Opcional. Se omitido, o script detecta o IP do ESP32 pela serial. Se informado e a ESP reportar outro IP, o da ESP prevalece e o script avisa. |
| `--token` | Token do bot. Se omitido, é lido de `src/secrets.h`. |
| `--chat-id` | Seu Chat ID. Se omitido, é lido de `src/secrets.h`. |
| `--link` | Link do painel enviado nas mensagens. Padrão: `http://<IP_DO_PC>:8000`. |
| `--cooldown` | Segundos de silêncio do Telegram após um alerta. Padrão: 300. |
| `--modelo` | Arquivo `.joblib` com a árvore treinada. Sem ele, o limiar é calculado a partir da calibração. |

O firmware imprime o IP do ESP32 na serial uma única vez, ao ligar. Se o script já estiver rodando quando a placa conectar, ou se a placa reiniciar ao abrir a porta serial, o IP é detectado sozinho. Caso contrário, reinicie o ESP32 com o script aberto ou use `--ip`.

---

## Comandos do Telegram

| Comando | Ação |
|---|---|
| `start` | Mostra a lista de comandos. |
| `ligar computador` | Envia o Wake-on-LAN. |
| `desligar computador` | Localiza o PC e executa o protocolo de desligamento. |
| `status` | Informa o estado do PC e do radar. |
| `ativar radar` | Arma os alertas de movimento. |
| `desativar radar` | Desarma os alertas. |

Somente o chat configurado em `chatID` é atendido. Outros usuários recebem "Acesso negado".

---

## Solução de problemas

| Sintoma | Causa provável e o que fazer |
|---|---|
| A calibração sobe até 100% e volta a 0% | Chegam menos de 20 pacotes por segundo. O terminal mostra `[calib] FALHOU`. Confira o `ping` para o ESP32, se PC e ESP32 estão na mesma rede, se o roteador não isola clientes e se o `--ip` está correto. |
| Fica em "Calibrando 0%" | Nenhuma linha válida chega da serial. Procure `[serial] nao consegui abrir`: feche monitores seriais e confira o `--serial` e o cabo. |
| O log mostra `UDP NAO iniciado` | O IP do ESP32 não foi detectado. Reinicie o ESP32 com o script aberto ou informe `--ip`. |
| Aviso `timestamp voltou no tempo` | O ESP32 reiniciou, por falta de energia no USB ou cabo ruim. A calibração recomeça. |
| Pacotes por segundo baixos ou instáveis | Interferência no 2.4 GHz, sinal fraco, VPN ou adaptadores virtuais (WSL, Docker) mudando a rota do PC. No PowerShell, `Find-NetRoute -RemoteIPAddress <IP_DO_ESP32>` mostra por qual interface o PC fala com a placa. |
| O painel fica em "Carregando..." | O navegador não alcança o servidor ou não tem internet para carregar o Chart.js. |
| O bot não responde | Confira `BOT_TOKEN` e `chatID` no `secrets.h`. |

### Log de diagnóstico

O painel grava `python/log_radar.txt` a cada 2 segundos, em todas as fases, com a fase atual, o progresso da calibração, os pacotes por segundo reais, o IP alvo do UDP e o número de falhas de calibração. Quando algo está errado, a linha inclui uma dica. O arquivo está no `.gitignore`.

---

## Limitações conhecidas

* O agente de controle (`escuta.py`) foi desenvolvido apenas para Windows.
* O sistema CSI detecta **movimento**, não presença estática. Pessoas imóveis geram pouca alteração no sinal.
* O painel web roda na rede local sem autenticação.
* Se o script Python do radar reiniciar, o alarme volta ao estado padrão (desarmado).
* O radar depende do tráfego entre o PC e o ESP32. Redes com isolamento de clientes, firewall entre as máquinas ou proteção contra flood de UDP podem impedir a calibração.
* O gráfico do painel carrega o Chart.js de um CDN e precisa de internet no navegador.
* As credenciais do Wi-Fi ficam no firmware (`secrets.h`). Para trocar de rede é preciso recompilar e gravar a placa.
* O limiar de sensibilidade é fixo (0,5 com o modelo). Para mudar, é preciso alterar o código.