# Telegram Bot ESP32 - Controle e Monitoramento de PC

Este projeto utiliza um microcontrolador **ESP32** integrado à API do Telegram para ligar, desligar e monitorar remotamente o status de um computador na rede local.

## Funcionalidades
* **Ligar Computador (Wake-on-LAN):** Envia um "Magic Packet" para ligar a máquina através do MAC Address.
* **Desligar Computador:** Comunicação via UDP com um script Python rodando no PC para executar o desligamento seguro.
* **Rastreamento Automático de IP:** A ESP32 descobre automaticamente o IP do PC na rede através de Broadcast UDP + Validação de MAC Address (dispensando a necessidade de IP Fixo).
* **Monitoramento por LED (TCP Ping):** A ESP32 verifica a cada 5 segundos se a porta 5000 do PC está aberta. Se o PC estiver ativo, o LED embutido acende. Se desligar, o LED apaga.
* **Segurança Reforçada:** O Bot só responde a comandos originados do seu `Chat ID` específico.

---

## Como Configurar e Instalar

### 1. Configuração da ESP32 (Ficheiro de Senhas)
Para proteger os seus dados, as senhas e tokens não ficam no código principal. 
Crie um ficheiro chamado **`secrets.h`** dentro da pasta `src/` e preencha com os seus dados:

```cpp
#ifndef SECRETS_H
#define SECRETS_H
#include <Arduino.h>

const char* const WIFI_SSID = "NOME_DA_SUA_REDE";
const char* const WIFI_PASSWORD = "SENHA_DA_REDE";

const char* const BOT_TOKEN = "SEU_TOKEN_DO_TELEGRAM"; 
const String CHAT_ID_PERMITIDO = "SEU_CHAT_ID"; 

// MAC Address da placa de rede do seu PC
const uint8_t MAC_ALVO[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

#endif

```

*(Nota: O ficheiro `secrets.h` está incluído no `.gitignore` para não ser enviado para o GitHub).*

### 2. Configuração do PC (Script Python)

O computador precisa rodar o script para ouvir os comandos da ESP32.

1. Instale a biblioteca necessária para capturar o MAC:
```bash
pip install getmac
```


2. Execute o ficheiro `escuta.py` na máquina. Ele abrirá dois ouvintes simultâneos na porta `5000` (UDP para receber comandos, TCP para manter o LED de status atualizado).

### 3. Liberação no Firewall do Windows

Para que a ESP32 consiga verificar se o PC está ligado (ping TCP), você precisa permitir a porta 5000 no Windows Defender.
Abra o **Prompt de Comando (como Administrador)** e rode:

```cmd
netsh advfirewall firewall add rule name="Libera TCP 5000 ESP" dir=in action=allow protocol=TCP localport=5000
```

---

## Comandos Disponíveis no Telegram

| Comando | Ação |
| --- | --- |
| `/start` | Inicia o bot e exibe uma mensagem de boas-vindas. |
| `ligar computador` | Envia o Wake-On-Lan e acende o LED manualmente. |
| `desligar computador` | Envia o comando de desligamento via UDP para o PC. |
| `ligar led` | Acende o LED (Ativa o Modo Manual, parando o rastreio automático). |
| `desligar led` | Apaga o LED (Ativa o Modo Manual, parando o rastreio automático). |

## Dependências da ESP32

* [UniversalTelegramBot](https://github.com/witnessmenow/Universal-Arduino-Telegram-Bot)
* [ArduinoJson](https://arduinojson.org/)
