#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include "esp_wifi.h"


static WiFiUDP csiUdp;

static unsigned long ultimo_ping_csi = 0;
// 20 ms = ~50 pacotes/s.
static const unsigned long intervalo_ping_csi = 20; 

static const int TAMANHO_CSI = 128;
static const int FILA_TAM = 64;
static const int FILA_MASCARA = FILA_TAM - 1;

struct AmostraCsi
{
  uint32_t timestamp;
  int8_t rssi;
  int8_t raw[TAMANHO_CSI];
  /* data */
};

static AmostraCsi fila[FILA_TAM];
static volatile uint16_t cabeca = 0;
static volatile uint16_t cauda = 0;      // próxima posição a ler (csiLoop)
static volatile uint32_t descartes = 0;  // pacotes perdidos por fila cheia

static portMUX_TYPE csiMux = portMUX_INITIALIZER_UNLOCKED;

// Linha de saida: ate 10 digitos + rssi + tamanho + 384 valores de ate 5 chars + \n
static char linhaBuf[2400];

static void csiCallback(void *ctx, wifi_csi_info_t *info) {
  if (!info || !info->buf) return;
  if (info->len < TAMANHO_CSI) return;   // só aceita pacote com LLTF completo

  portENTER_CRITICAL(&csiMux);
  if ((uint16_t)(cabeca - cauda) >= FILA_TAM) {
    descartes = descartes + 1;           // fila cheia: descarta o pacote novo
  } else {
    AmostraCsi &a = fila[cabeca & FILA_MASCARA];
    memcpy(a.raw, info->buf, TAMANHO_CSI);
    a.rssi = info->rx_ctrl.rssi;
    a.timestamp = millis();
    cabeca = (uint16_t)(cabeca + 1);
  }
  portEXIT_CRITICAL(&csiMux);
}

// Escreve um inteiro em texto no destino e devolve quantos chars escreveu.
static int escreveInt(char *dst, long v) {
  char tmp[12];
  int n = 0;
  bool neg = v < 0;
  unsigned long u = neg ? (unsigned long)(-v) : (unsigned long)v;

  do {
    tmp[n++] = (char)('0' + (u % 10));
    u /= 10;
  } while (u > 0);

  int pos = 0;
  if (neg) dst[pos++] = '-';
  while (n > 0) dst[pos++] = tmp[--n];
  return pos;
}

void csiSetup() {
  // Desliga a economia de energia do WiFi: com modem sleep a recepcao fica irregular.
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

  Serial.println("CSI ativado");
  Serial.println("timestamp_ms,rssi,tamanho,csi_raw");
}

void csiLoop() {
  unsigned long agora = millis();

  if (agora - ultimo_ping_csi >= intervalo_ping_csi) {
    ultimo_ping_csi = agora;
    csiUdp.beginPacket(WiFi.gatewayIP(), 9); // pinga o roteador so pra gerar trafego
    csiUdp.write((uint8_t)0);
    csiUdp.endPacket();
  }

  // TEMPORÁRIO: só drena a fila e conta pacotes por segundo
  static uint32_t contados = 0;
  static unsigned long t0 = 0;

  for (;;) {
    portENTER_CRITICAL(&csiMux);
    bool vazio = (cabeca == cauda);
    if (!vazio) cauda = (uint16_t)(cauda + 1);
    portEXIT_CRITICAL(&csiMux);
    if (vazio) break;
    contados++;
  }

  if (agora - t0 >= 1000) {
    Serial.printf("pps=%lu descartes=%lu\n", (unsigned long)contados, (unsigned long)descartes);
    contados = 0;
    t0 = agora;
  }
}