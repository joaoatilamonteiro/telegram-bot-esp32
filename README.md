# Telegram Bot ESP32 + Radar Wi-Fi (CSI)

Este projeto transforma um ESP32 em um assistente residencial controlado via Telegram. Ele atua como ponte para controlar o seu computador pela rede local e, ao mesmo tempo, funciona como um **radar de movimento invisível**: usa a distorção do sinal Wi-Fi (CSI, Channel State Information) para detectar movimento no ambiente e enviar alertas.

O radar roda **inteiro no ESP32**: captura, calibração, classificação, alertas e painel web. Não é preciso computador ligado nem Raspberry Pi para o radar funcionar.

## Funcionalidades

**Controle do computador (PC)**

* `ligar computador`: envia um pacote mágico (Wake-on-LAN) para ligar o PC remotamente.
* `desligar computador`: localiza o PC na rede por broadcast UDP, valida o MAC Address e envia o comando de desligamento ao agente `escuta.py`. O ESP32 tenta até 5 vezes e informa no Telegram se o PC parou de responder.

**Radar CSI (detecção de movimento por Wi-Fi)**

* `ativar radar` e `desativar radar`: armam e desarmam os alertas pelo Telegram.
* `recalibrar radar`: refaz a calibração de 60 segundos (útil quando o ambiente mudou, por exemplo móveis de lugar).
* `status`: informa se o PC está online (com o IP) e o estado do radar: armado ou desarmado, situação (calibrando, sem movimento ou movimento), pacotes por segundo, horário do último alerta e o link do painel.
* **Sem câmeras:** o ESP32 mede o CSI de cada resposta do roteador no núcleo 0 e decide sozinho, com uma árvore de decisão embutida no firmware.
* **Painel web ao vivo (servido pelo próprio ESP32):** situação atual, probabilidade de movimento e limiar, as características usadas pela árvore (`osc_p90` e `energia`), a oscilação de cada subportadora, o caminho percorrido na árvore e diagnósticos (pacotes por segundo, memória livre).
* **Alertas com cooldown:** notificações no Telegram com intervalo de silêncio (padrão de 5 min) e contagem dos picos registrados nesse intervalo.

---

## Estrutura do projeto

```
telegram_bot/
├── platformio.ini              Configuração do PlatformIO
├── README.md
├── src/
│   ├── main.cpp                Bot do Telegram, Wake-on-LAN, alertas e criação das tarefas
│   ├── csi_radar.cpp           Radar (núcleo 0): captura do CSI, tráfego ICMP, calibração, decisão
│   ├── radar_features.h        Calibração e características do CSI (osc_p90 e energia)
│   ├── radar_decisao.h         Árvore embutida, regra de alerta (4 de 10) e cooldown
│   ├── radar_api.h             Estado compartilhado entre o radar (núcleo 0) e o resto (núcleo 1)
│   ├── painel_web.h / .cpp     Painel web servido pelo ESP32
│   ├── modelo_arvore.h         Árvore treinada convertida para C++ (GERADO, não editar à mão)
│   └── secrets.h               Credenciais (ignorado pelo git)
└── python/
    ├── escuta.py               Agente do PC (Windows): responde à busca e executa o desligamento
    ├── csi_features.py         Calibração e características (referência do treino)
    ├── modelo_arvore.joblib    Árvore de decisão treinada (movimento / sem movimento)
    ├── exportar_modelo_h.py    Converte o .joblib em src/modelo_arvore.h
    ├── demo_ao_vivo_web.py     LEGADO: painel em Python, só funciona com o firmware antigo (CSV pela serial)
    └── requirements.txt        Dependências Python
```

---

## Como o radar funciona

1. O ESP32 envia um **ping ICMP ao roteador** a cada 20 ms. As respostas são pacotes recebidos, e é neles que o CSI é medido (cerca de 40 a 50 por segundo).
2. O callback do CSI copia as 64 subportadoras (LLTF) de cada pacote para uma fila circular. A task do radar (núcleo 0) converte em amplitudes.
3. Nos primeiros **60 segundos**, com o ambiente parado, o radar aprende o "normal": quais subportadoras são úteis (máscara, definida nos 5 primeiros segundos) e a oscilação típica de cada uma. Se chegarem pacotes de menos, a calibração recomeça.
4. Depois, a cada 0,5 s ele avalia a janela do último 1 s (mínimo de 20 pacotes) e calcula `osc_p90` e `energia`.
5. A **árvore de decisão** embutida (`modelo_arvore.h`) transforma as duas características em uma probabilidade de movimento. O limiar é 0,5.
6. O alerta é ativado quando pelo menos 4 das últimas 10 janelas passam do limiar. Ele só vai ao Telegram se o radar estiver armado e fora do cooldown. Quem envia a mensagem é o `loop()` do núcleo 1, nunca a task do radar.

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

* A comunicação serial roda a **115200 baud**.
* O ESP32 só usa a banda de 2.4 GHz. A rede configurada precisa oferecê-la.
* A rede precisa aceitar WPA/WPA2 pessoal. Redes corporativas (WPA2 Enterprise) e com portal de login não são suportadas.
* O roteador precisa **responder a ping** (ICMP). É isso que gera os pacotes do radar.
* Para ligar o PC remotamente, o Wake-on-LAN precisa estar habilitado na BIOS e na placa de rede.

### 2. Agente do PC (Windows) — só para ligar/desligar o computador

O radar não depende dele. Só o controle do PC precisa do `escuta.py`:

```powershell
python -m venv python\.venv
python\.venv\Scripts\activate
pip install getmac
python python/escuta.py
```

Libere a porta UDP/TCP 5000 no firewall do Windows.

---

## Usando o radar

1. Alimente o ESP32 e abra o monitor serial (`pio device monitor`). Quando o Wi-Fi conectar, ele mostra `Painel do radar: http://<IP>/`.
2. Abra esse endereço no navegador do PC ou do celular, na mesma rede.
3. Mantenha a área **parada por 60 segundos** até aparecer "SEM MOVIMENTO". Na serial você vê `Mascara definida`, `Calibracao concluida` e uma linha `janela:` a cada 0,5 s.
4. No Telegram, envie `ativar radar` para armar os alertas.

Valores de referência (parado): `osc_p90` em torno de 1,1 a 1,2 e `energia` perto de 1. Com alguém se movendo, `osc_p90` passa de 1,4. Por ser um percentil 90, o `osc_p90` fica um pouco acima de 1 mesmo sem movimento.

---

## Atualizando o modelo

O treino continua no PC. A ESP32 só executa a árvore.

1. Treine a árvore e salve o `.joblib` (dicionário com `modelo`, `features` e `versao`), usando **apenas** as características `osc_p90` e `energia`, que são as que o firmware calcula.
2. Converta para C++ (precisa de `numpy`, `scikit-learn` e `joblib`):

   ```powershell
   python python/exportar_modelo_h.py
   ```

   O script valida a conversão contra o scikit-learn e aborta se divergir ou se a árvore usar outra característica.
3. Regrave o firmware: `pio run -t upload`.

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
| `recalibrar radar` | Refaz a calibração de 60 s. |

Somente o chat configurado em `chatID` é atendido. Outros usuários recebem "Acesso negado".

---

## Solução de problemas

| Sintoma | Causa provável e o que fazer |
|---|---|
| A calibração sobe até 100% e volta a 0% | Chegam menos de 20 pacotes por janela. A serial mostra `Calibracao FALHOU`. Confira se o roteador responde a ping e a linha `pps=` na serial. |
| `pps` perto de 10 | O roteador não está respondendo ao ping e só chegam os beacons. Teste outro roteador ou reduza filtros de ICMP. |
| `pps` baixo ou instável | Interferência no 2.4 GHz ou sinal fraco. Aproxime o ESP32 do roteador. |
| `ERRO: sessao de ping falhou` | O ESP32 não obteve o gateway. Verifique o Wi-Fi; o radar tenta de novo a cada 2 s. |
| O painel mostra "Sem resposta da ESP32..." | O navegador não alcança o IP da placa (outra rede, ou isolamento de clientes no roteador). |
| `parado` e `movendo` aparecem na mesma faixa de valores | O tráfego (respostas do roteador) pode ter características diferentes do usado no treino. Recoletar dados com esse tráfego e reexportar a árvore. |
| O bot não responde | Confira `BOT_TOKEN` e `chatID` no `secrets.h`. |
| O alerta demora a chegar | O `loop()` pode estar ocupado (por exemplo, no protocolo de desligamento, que dura cerca de 1 minuto). O aviso sai assim que ele terminar. |

---

## Limitações conhecidas

* O agente de controle (`escuta.py`) foi desenvolvido apenas para Windows.
* O sistema CSI detecta **movimento**, não presença estática. Pessoas imóveis geram pouca alteração no sinal.
* O painel web roda na rede local sem autenticação. Ele só exibe dados; armar, desarmar e recalibrar são feitos só pelo Telegram.
* O alarme volta a **desarmado** quando o ESP32 reinicia.
* A calibração é refeita a cada reinício (60 s) e não é salva.
* O radar depende de o roteador responder a ping. Roteadores que bloqueiam ICMP impedem a captura.
* A hora das mensagens vem de NTP (fuso fixo UTC-3). Sem internet, as mensagens trazem "em 5 min" no lugar do horário.
* As credenciais do Wi-Fi ficam no firmware (`secrets.h`). Para trocar de rede é preciso recompilar e gravar a placa.
* O limiar de sensibilidade é fixo (0,5). Para mudar, é preciso alterar `LIMIAR_PROB` em `radar_decisao.h`.
* `demo_ao_vivo_web.py` não funciona com este firmware, porque o ESP32 não envia mais o CSV pela serial. Foi mantido só como referência.
