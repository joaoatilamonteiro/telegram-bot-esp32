#pragma once
// Ponte entre a task do radar (core 0) e o resto do firmware (loop, Telegram e painel web, core 1).
#include <stdint.h>

static const int RADAR_HIST = 60;                 // 60 janelas de 0,5 s = 30 s de historico
static const uint32_t RADAR_COOLDOWN_S = 300;     // silencio entre avisos do Telegram
static const uint8_t RADAR_FASE_AO_VIVO = 2;      // 0 e 1 = calibrando

// Copia consistente do estado do radar (todos os campos do mesmo instante).
struct EstadoRadar {
  uint8_t fase;
  float prog;               // 0..1 durante a calibracao
  bool tem_valor;           // false enquanto nao ha janela recente
  float valor;              // probabilidade de movimento (arvore)
  float osc_p90;
  float energia;
  float limiar;
  bool alerta;              // regra 4 de 10 atendida agora
  bool armado;
  uint16_t pps;             // pacotes na janela de 1 s
  uint16_t k;               // subportadoras usadas
  uint16_t falhas;          // calibracoes que recomecaram
  uint32_t descartes;       // pacotes perdidos por fila cheia
  uint16_t n_hist;
  float hist_valor[RADAR_HIST];     // do mais antigo para o mais novo
  float hist_osc[RADAR_HIST];
  float hist_energia[RADAR_HIST];
  float z[64];              // oscilacao relativa por subportadora (0 = descartada)
  char caminho[120];        // caminho percorrido na arvore
};

// Task do radar (core 0)
void csiSetup();
void csiLoop();

// Podem ser chamadas de qualquer task.
void radarCopiaEstado(EstadoRadar &out);
void radarDefineArmado(bool armado);
bool radarEstaArmado();
void radarPedeRecalibracao();               // a task do radar recalibra no proximo ciclo

// Devolve true uma unica vez por aviso a enviar; extras = picos registrados no cooldown anterior.
bool radarConsomeAlerta(uint16_t &extras);
