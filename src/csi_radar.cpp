#include <Arduino.h>
#include <WiFi.h>
#include <math.h>
#include "esp_wifi.h"
#include "ping/ping_sock.h"
#include "lwip/ip_addr.h"
#include "radar_features.h"


static const uint32_t INTERVALO_PING_MS = 20;


static const int TAMANHO_CSI = 128;        
static const int FILA_TAM = 64;            
static const int FILA_MASCARA = FILA_TAM - 1;

struct AmostraCsi {
  uint32_t timestamp;
  int8_t rssi;
  int8_t raw[TAMANHO_CSI];
};

static AmostraCsi fila[FILA_TAM];
static volatile uint16_t cabeca = 0;      
static volatile uint16_t cauda = 0;        
static volatile uint32_t descartes = 0;    

static portMUX_TYPE csiMux = portMUX_INITIALIZER_UNLOCKED;

static radar::Detector det;

static void csiCallback(void *ctx, wifi_csi_info_t *info) {
  (void)ctx;
  if (!info || !info->buf) return;
  if ((int)info->len < TAMANHO_CSI) return;   
  portENTER_CRITICAL(&csiMux);
  if ((uint16_t)(cabeca - cauda) >= FILA_TAM) {
    descartes = descartes + 1;                
  } else {
    AmostraCsi &a = fila[cabeca & FILA_MASCARA];
    memcpy(a.raw, info->buf, TAMANHO_CSI);
    a.rssi = info->rx_ctrl.rssi;
    a.timestamp = millis();
    cabeca = (uint16_t)(cabeca + 1);
  }
  portEXIT_CRITICAL(&csiMux);
}

static void iniciaPingGateway() {
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
  cfg.timeout_ms = 500;
  cfg.data_size = 8;

  esp_ping_callbacks_t cbs = {};
  esp_ping_handle_t handle = NULL;
  if (esp_ping_new_session(&cfg, &cbs, &handle) != ESP_OK ||
      esp_ping_start_session(handle) != ESP_OK) {
    Serial.println("ERRO: sessao de ping falhou");
    return;
  }

  Serial.print("Ping ICMP iniciado para o gateway ");
  Serial.println(gw);
}

void csiSetup() {
  WiFi.setSleep(false);

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
  Serial.println("CSI ativado. Mantenha a area parada durante a calibracao (60 s).");
}

void csiLoop() {
  unsigned long agora = millis();

  static uint32_t contados = 0;
  static unsigned long t0 = 0;
  static radar::Fase faseAnt = radar::CALIBRANDO_MASCARA;
  static uint16_t falhasAnt = 0;

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

    float amp[radar::N_SUB];
    for (int k = 0; k < radar::N_SUB; k++) {
      float p = (float)a.raw[2 * k];
      float q = (float)a.raw[2 * k + 1];
      amp[k] = sqrtf(p * p + q * q);
    }

    if (det.adicionar(a.timestamp, amp)) {
      const radar::Resultado &r = det.ultimo();
      Serial.printf("janela: osc_p90=%.2f energia=%.2f pacotes=%u\n",
                    r.osc_p90, r.energia, (unsigned)r.n_pacotes);
    }
  }

  if (det.falhasCalibracao() != falhasAnt) {
    falhasAnt = det.falhasCalibracao();
    faseAnt = det.fase();
    Serial.println("Calibracao FALHOU (poucos pacotes por janela?). Reiniciando.");
  } else if (det.fase() != faseAnt) {
    faseAnt = det.fase();
    if (faseAnt == radar::CALIBRANDO_JANELAS) {
      Serial.printf("Mascara definida: %d subportadoras.\n", det.subportadorasUsadas());
    } else if (faseAnt == radar::AO_VIVO) {
      Serial.printf("Calibracao concluida (%d janelas). Radar ao vivo.\n", det.janelasCalibracao());
    }
  }

  if (agora - t0 >= 1000) {
    t0 = agora;
    if (det.fase() == radar::AO_VIVO) {
      Serial.printf("pps=%lu descartes=%lu\n", (unsigned long)contados, (unsigned long)descartes);
    } else {
      Serial.printf("calibrando %d%% | pps=%lu descartes=%lu\n",
                    (int)(det.progresso() * 100.0f), (unsigned long)contados, (unsigned long)descartes);
    }
    contados = 0;
  }
}