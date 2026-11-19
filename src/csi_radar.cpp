#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <math.h>
#include <string.h>
#include "esp_wifi.h"
#include "ping/ping_sock.h"
#include "lwip/ip_addr.h"
#include "esp_idf_version.h"
#include "radar_features.h"
#include "radar_decisao.h"
#include "radar_api.h"

// ---------------------------------------------------------------------------
// Trafego: ping ICMP ao roteador. Cada resposta e um pacote recebido, e e nele
// que o CSI e medido. A taxa real fica um pouco abaixo de 1000/INTERVALO_PING_MS.
// ---------------------------------------------------------------------------
static const uint32_t INTERVALO_PING_MS = 10;

// Segunda fonte de trafego: consulta DNS ao roteador (UDP 53), enviada a intervalo fixo sem
// esperar a resposta. Nao depende de o roteador responder ICMP nem do tempo de ida e volta.
static const uint32_t INTERVALO_DNS_MS = 20;
static const uint8_t DNS_CONSULTA[] = {
  0x12, 0x34,                                      // id
  0x01, 0x00,                                      // flags: consulta recursiva
  0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // 1 pergunta
  0x01, 'a', 0x00,                                 // nome "a"
  0x00, 0x01,                                      // tipo A
  0x00, 0x01                                       // classe IN
};
static WiFiUDP dnsUdp;

// O nome das funcoes de iniciar/parar o ping mudou entre versoes do ESP-IDF.
// O Arduino core 2.x (IDF 4.4) usa esp_ping_start/esp_ping_stop.
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
  #define PING_INICIA(h) esp_ping_start_session(h)
  #define PING_PARA(h)   esp_ping_stop_session(h)
#else
  #define PING_INICIA(h) esp_ping_start(h)
  #define PING_PARA(h)   esp_ping_stop(h)
#endif

// ---------------------------------------------------------------------------
// Fila circular entre o callback (task do WiFi) e o csiLoop() (task do radar).
// ---------------------------------------------------------------------------
static const int TAMANHO_CSI = 128;        // 64 subportadoras do LLTF x 2 bytes
static const int FILA_TAM = 64;            // precisa ser potencia de 2
static const int FILA_MASCARA = FILA_TAM - 1;

struct AmostraCsi {
  uint32_t timestamp;
  int8_t rssi;
  int8_t raw[TAMANHO_CSI];
};

static AmostraCsi fila[FILA_TAM];
static volatile uint16_t cabeca = 0;       // proxima posicao a escrever (callback)
static volatile uint16_t cauda = 0;        // proxima posicao a ler (csiLoop)
static volatile uint32_t descartes = 0;    // pacotes perdidos por fila cheia

static portMUX_TYPE csiMux = portMUX_INITIALIZER_UNLOCKED;

// ---------------------------------------------------------------------------
// Calibracao, features e decisao. Usados so pela task do radar (core 0).
// ---------------------------------------------------------------------------
static radar::Detector det;
static radar::Decisor dec;

// ---------------------------------------------------------------------------
// Estado publicado para o resto do firmware (core 1). Protegido por estadoMux.
// ---------------------------------------------------------------------------
static EstadoRadar estado;
static portMUX_TYPE estadoMux = portMUX_INITIALIZER_UNLOCKED;
static bool alertaPendente = false;        // protegido por estadoMux
static uint16_t extrasPendente = 0;        // protegido por estadoMux

static volatile bool armado = false;       // escrito pelo Telegram, lido pelo radar
static volatile bool pedidoRecalibrar = false;
static unsigned long ultimoResultadoMs = 0;

static_assert(sizeof(((EstadoRadar *)0)->z) == sizeof(((radar::Resultado *)0)->z),
              "EstadoRadar::z precisa ter o tamanho de Resultado::z");

// ---------------------------------------------------------------------------
// Ping ao gateway
// ---------------------------------------------------------------------------
static esp_ping_handle_t pingHandle = NULL;
static IPAddress gwAtual;

static void paraPing() {
  if (pingHandle) {
    PING_PARA(pingHandle);
    esp_ping_delete_session(pingHandle);
    pingHandle = NULL;
  }
  gwAtual = IPAddress();
}

// Diagnostico do ping (escritos pela task do ping, lidos e zerados pelo csiLoop).
static volatile uint32_t pingOk = 0, pingTimeout = 0, pingRttSoma = 0;

static void aoPingSucesso(esp_ping_handle_t hdl, void *args) {
  (void)args;
  uint32_t t = 0;
  esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &t, sizeof(t));
  pingOk = pingOk + 1;
  pingRttSoma = pingRttSoma + t;
}

static void aoPingTimeout(esp_ping_handle_t hdl, void *args) {
  (void)hdl;
  (void)args;
  pingTimeout = pingTimeout + 1;
}

static void iniciaPingGateway() {
  paraPing();

  IPAddress gw = WiFi.gatewayIP();
  if (gw[0] == 0) {
    Serial.println("ERRO: sem gateway, ping nao iniciado");
    return;
  }

  ip_addr_t alvo;
  IP_ADDR4(&alvo, gw[0], gw[1], gw[2], gw[3]);

  esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
  cfg.target_addr = alvo;
  cfg.count = ESP_PING_COUNT_INFINITE;
  cfg.interval_ms = INTERVALO_PING_MS;
  cfg.timeout_ms = 50;     // resposta perdida bloqueia o proximo ping so por este tempo
  cfg.data_size = 8;

  esp_ping_callbacks_t cbs = {};
  cbs.on_ping_success = aoPingSucesso;
  cbs.on_ping_timeout = aoPingTimeout;
  esp_ping_handle_t handle = NULL;
  if (esp_ping_new_session(&cfg, &cbs, &handle) != ESP_OK ||
      PING_INICIA(handle) != ESP_OK) {
    Serial.println("ERRO: sessao de ping falhou");
    return;
  }

  pingHandle = handle;
  gwAtual = gw;
  Serial.print("Ping ICMP iniciado para o gateway ");
  Serial.println(gw);
}

// Se o Wi-Fi reconectou com outro gateway (ou o ping nunca subiu), recria a sessao.
static void verificaGateway() {
  if (WiFi.status() != WL_CONNECTED) return;
  IPAddress gw = WiFi.gatewayIP();
  if (gw[0] == 0 || gw == gwAtual) return;
  iniciaPingGateway();
}

// ---------------------------------------------------------------------------
// Callback do CSI (roda na task do WiFi: so copia e sai)
// ---------------------------------------------------------------------------
static void csiCallback(void *ctx, wifi_csi_info_t *info) {
  (void)ctx;
  if (!info || !info->buf) return;
  if ((int)info->len < TAMANHO_CSI) return;   // so aceita pacote com LLTF completo

  portENTER_CRITICAL(&csiMux);
  if ((uint16_t)(cabeca - cauda) >= FILA_TAM) {
    descartes = descartes + 1;                // fila cheia: descarta o pacote novo
  } else {
    AmostraCsi &a = fila[cabeca & FILA_MASCARA];
    memcpy(a.raw, info->buf, TAMANHO_CSI);
    a.rssi = info->rx_ctrl.rssi;
    a.timestamp = millis();
    cabeca = (uint16_t)(cabeca + 1);
  }
  portEXIT_CRITICAL(&csiMux);
}

// ---------------------------------------------------------------------------
// Publicacao do estado
// ---------------------------------------------------------------------------
static void empurra(float *arr, int n, float v) {
  if (n < RADAR_HIST) {
    arr[n] = v;
  } else {
    memmove(arr, arr + 1, (RADAR_HIST - 1) * sizeof(float));
    arr[RADAR_HIST - 1] = v;
  }
}

static void limpaEstadoJanela() {
  portENTER_CRITICAL(&estadoMux);
  estado.tem_valor = false;
  estado.alerta = false;
  estado.n_hist = 0;
  memset(estado.z, 0, sizeof(estado.z));
  estado.caminho[0] = '\0';
  portEXIT_CRITICAL(&estadoMux);
}

static void publicaBasico(unsigned long agora) {
  portENTER_CRITICAL(&estadoMux);
  estado.fase = (uint8_t)det.fase();
  estado.prog = det.progresso();
  estado.pps = (uint16_t)det.pacotesNaJanela();
  estado.k = (uint16_t)det.subportadorasUsadas();
  estado.falhas = det.falhasCalibracao();
  estado.descartes = descartes;
  estado.limiar = radar::LIMIAR_PROB;
  if (agora - ultimoResultadoMs > 2000) estado.tem_valor = false;
  estado.alerta = (det.fase() == radar::AO_VIVO) && dec.alerta() && estado.tem_valor;
  portEXIT_CRITICAL(&estadoMux);
}

static void publicaJanela(const radar::Resultado &r, float valor, const char *caminho) {
  size_t n = strlen(caminho);
  if (n >= sizeof(estado.caminho)) n = sizeof(estado.caminho) - 1;

  portENTER_CRITICAL(&estadoMux);
  estado.tem_valor = true;
  estado.valor = valor;
  estado.osc_p90 = r.osc_p90;
  estado.energia = r.energia;
  estado.alerta = dec.alerta();
  memcpy(estado.z, r.z, sizeof(estado.z));
  memcpy(estado.caminho, caminho, n);
  estado.caminho[n] = '\0';
  empurra(estado.hist_valor, estado.n_hist, valor);
  empurra(estado.hist_osc, estado.n_hist, r.osc_p90);
  empurra(estado.hist_energia, estado.n_hist, r.energia);
  if (estado.n_hist < RADAR_HIST) estado.n_hist++;
  portEXIT_CRITICAL(&estadoMux);
}

// ---------------------------------------------------------------------------
// API para o resto do firmware
// ---------------------------------------------------------------------------
void radarCopiaEstado(EstadoRadar &out) {
  portENTER_CRITICAL(&estadoMux);
  out = estado;
  portEXIT_CRITICAL(&estadoMux);
  out.armado = armado;
}

void radarDefineArmado(bool valor) { armado = valor; }
bool radarEstaArmado() { return armado; }
void radarPedeRecalibracao() { pedidoRecalibrar = true; }

bool radarConsomeAlerta(uint16_t &extras) {
  bool v;
  portENTER_CRITICAL(&estadoMux);
  v = alertaPendente;
  if (v) {
    alertaPendente = false;
    extras = extrasPendente;
  }
  portEXIT_CRITICAL(&estadoMux);
  return v;
}

// ---------------------------------------------------------------------------
// Setup e loop da task do radar
// ---------------------------------------------------------------------------
void csiSetup() {
  // Desliga a economia de energia do WiFi: com modem sleep a recepcao fica irregular.
  WiFi.setSleep(false);

  // Configuracao igual a da captura que gerou o modelo: nao mexer.
  wifi_csi_config_t csiConfig = {
    .lltf_en = true,
    .htltf_en = true,
    .stbc_htltf2_en = true,
    .ltf_merge_en = true,
    .channel_filter_en = true,
    .manu_scale = false,
    .shift = false,
  };

  if (esp_wifi_set_csi_config(&csiConfig) != ESP_OK) {
    Serial.println("ERRO: esp_wifi_set_csi_config falhou");
  }
  if (esp_wifi_set_csi_rx_cb(csiCallback, NULL) != ESP_OK) {
    Serial.println("ERRO: esp_wifi_set_csi_rx_cb falhou");
  }
  if (esp_wifi_set_csi(true) != ESP_OK) {
    Serial.println("ERRO: esp_wifi_set_csi falhou");
  }

  iniciaPingGateway();
  publicaBasico(millis());
  Serial.println("CSI ativado. Mantenha a area parada durante a calibracao (60 s).");
}

void csiLoop() {
  unsigned long agora = millis();

  static uint32_t contados = 0;
  static unsigned long t0 = 0, tGw = 0, ultimoDns = 0;
  static uint32_t dnsRespostas = 0;
  static radar::Fase faseAnt = radar::CALIBRANDO_MASCARA;
  static uint16_t falhasAnt = 0;

  // Pedido de recalibracao vindo do Telegram: so esta task mexe no detector.
  if (pedidoRecalibrar) {
    pedidoRecalibrar = false;
    det.reiniciar();
    dec.reiniciar();
    limpaEstadoJanela();
    faseAnt = det.fase();
    falhasAnt = det.falhasCalibracao();
    publicaBasico(agora);
    Serial.println("Recalibrando (60 s). Mantenha a area parada.");
  }

  if (agora - tGw >= 2000) {
    tGw = agora;
    verificaGateway();
  }

  // Trafego DNS: uma consulta a cada INTERVALO_DNS_MS. As respostas so sao contadas e descartadas
  // (o CSI e medido pelo driver, mesmo que ninguem leia o datagrama).
  if (WiFi.status() == WL_CONNECTED && agora - ultimoDns >= INTERVALO_DNS_MS) {
    ultimoDns = agora;
    dnsUdp.beginPacket(WiFi.gatewayIP(), 53);
    dnsUdp.write(DNS_CONSULTA, sizeof(DNS_CONSULTA));
    dnsUdp.endPacket();
    while (dnsUdp.parsePacket() > 0) {
      dnsUdp.flush();
      dnsRespostas++;
    }
  }

  // Drena a fila: copia a amostra sob trava, so depois libera a posicao.
  AmostraCsi a;
  for (;;) {
    bool vazio;
    portENTER_CRITICAL(&csiMux);
    vazio = (cabeca == cauda);
    if (!vazio) {
      a = fila[cauda & FILA_MASCARA];
      cauda = (uint16_t)(cauda + 1);
    }
    portEXIT_CRITICAL(&csiMux);
    if (vazio) break;
    contados++;

    // Amplitude de cada subportadora: o buffer vem em pares (imaginario, real).
    float amp[radar::N_SUB];
    for (int k = 0; k < radar::N_SUB; k++) {
      float p = (float)a.raw[2 * k];
      float q = (float)a.raw[2 * k + 1];
      amp[k] = sqrtf(p * p + q * q);
    }

    if (det.adicionar(a.timestamp, amp)) {
      const radar::Resultado &r = det.ultimo();

      float f[ARV_N_FEATURES] = {0};
      f[FEAT_OSC_P90] = r.osc_p90;
      f[FEAT_ENERGIA] = r.energia;
      float valor = radar::arvoreProb(f);
      dec.atualizar(valor);

      char caminho[120];
      radar::arvoreCaminho(f, caminho, sizeof(caminho));

      if (dec.deveEnviar(armado, millis(), RADAR_COOLDOWN_S * 1000UL)) {
        portENTER_CRITICAL(&estadoMux);
        alertaPendente = true;
        extrasPendente = dec.extrasMsg();
        portEXIT_CRITICAL(&estadoMux);
      }

      ultimoResultadoMs = millis();
      publicaJanela(r, valor, caminho);
      Serial.printf("janela: osc_p90=%.2f energia=%.2f valor=%.2f alerta=%d pacotes=%u\n",
                    r.osc_p90, r.energia, valor, dec.alerta() ? 1 : 0, (unsigned)r.n_pacotes);
    }
  }

  // Avisos de mudanca de fase da calibracao.
  if (det.falhasCalibracao() != falhasAnt) {
    falhasAnt = det.falhasCalibracao();
    faseAnt = det.fase();
    Serial.println("Calibracao FALHOU (poucos pacotes por janela?). Reiniciando.");
    publicaBasico(agora);
  } else if (det.fase() != faseAnt) {
    faseAnt = det.fase();
    if (faseAnt == radar::CALIBRANDO_JANELAS) {
      Serial.printf("Mascara definida: %d subportadoras.\n", det.subportadorasUsadas());
    } else if (faseAnt == radar::AO_VIVO) {
      Serial.printf("Calibracao concluida (%d janelas). Radar ao vivo.\n", det.janelasCalibracao());
    }
    publicaBasico(agora);
  }

  if (agora - t0 >= 1000) {
    t0 = agora;
    publicaBasico(agora);
    if (det.fase() == radar::AO_VIVO) {
      Serial.printf("pps=%lu descartes=%lu\n", (unsigned long)contados, (unsigned long)descartes);
    } else {
      Serial.printf("calibrando %d%% | pps=%lu descartes=%lu\n",
                    (int)(det.progresso() * 100.0f), (unsigned long)contados, (unsigned long)descartes);
    }
    contados = 0;

    uint32_t ok = pingOk, tmo = pingTimeout, rtt = pingRttSoma;
    pingOk = 0;
    pingTimeout = 0;
    pingRttSoma = 0;
    Serial.printf("ping ok=%lu timeout=%lu rtt_medio=%lu ms\n",
                  (unsigned long)ok, (unsigned long)tmo, (unsigned long)(ok ? rtt / ok : 0));
    Serial.printf("dns respostas=%lu\n", (unsigned long)dnsRespostas);
    dnsRespostas = 0;
  }
}