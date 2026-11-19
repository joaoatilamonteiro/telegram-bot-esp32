#pragma once
// Decisao do radar: arvore treinada (modelo_arvore.h) + regra de alerta da demo Python.
// C++ puro (sem Arduino/ESP-IDF).
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include "modelo_arvore.h"

namespace radar {

static const float LIMIAR_PROB = 0.5f;   // limiar padrao da demo (sensibilidade 1.0)
static const int FLAGS_N = 10;           // quantas janelas recentes entram na regra
static const int FLAGS_M = 4;            // quantas delas precisam passar do limiar

// Probabilidade de movimento. f tem ARV_N_FEATURES posicoes; so as usadas pela
// arvore (FEAT_OSC_P90 e FEAT_ENERGIA) precisam estar preenchidas.
inline float arvoreProb(const float *f) {
  int n = 0;
  while (ARV_ESQ[n] != -1) {
    n = (f[ARV_FEAT[n]] <= ARV_LIM[n]) ? ARV_ESQ[n] : ARV_DIR[n];
  }
  return ARV_PROB[n];
}

inline const char *nomeFeature(int i) {
  if (i == FEAT_OSC_P90) return "osc_p90";
  if (i == FEAT_ENERGIA) return "energia";
  return "?";
}

// Texto com o caminho percorrido na arvore, para o painel. Ex.:
// "osc_p90 1.62 > 1.4140 -> osc_p90 1.62 > 1.5885 -> energia 1.10 <= 3.0785 -> prob 0.98"
inline void arvoreCaminho(const float *f, char *buf, size_t tam) {
  if (tam == 0) return;
  buf[0] = '\0';
  size_t pos = 0;
  int n = 0;
  while (ARV_ESQ[n] != -1) {
    int ft = ARV_FEAT[n];
    bool esq = f[ft] <= ARV_LIM[n];
    int w = snprintf(buf + pos, tam - pos, "%s %.2f %s %.4f -> ",
                     nomeFeature(ft), f[ft], esq ? "<=" : ">", ARV_LIM[n]);
    if (w < 0 || (size_t)w >= tam - pos) return;   // sem espaco: fica o que coube
    pos += (size_t)w;
    n = esq ? ARV_ESQ[n] : ARV_DIR[n];
  }
  snprintf(buf + pos, tam - pos, "prob %.2f", ARV_PROB[n]);
}

// Regra de alerta: pelo menos FLAGS_M das ultimas FLAGS_N janelas acima do limiar.
// Envio ao Telegram: so se armado e fora do cooldown; picos durante o cooldown sao contados.
class Decisor {
 public:
  Decisor() { reiniciar(); }

  // Limpa as janelas recentes (ex.: ao recalibrar). Mantem o cooldown.
  void reiniciar() {
    for (int i = 0; i < FLAGS_N; i++) flags_[i] = false;
    pos_ = 0;
    alerta_ = false;
  }

  void atualizar(float valor) {
    flags_[pos_] = valor > LIMIAR_PROB;
    pos_ = (pos_ + 1) % FLAGS_N;
    int soma = 0;
    for (int i = 0; i < FLAGS_N; i++) soma += flags_[i] ? 1 : 0;
    alerta_ = soma >= FLAGS_M;
  }

  bool alerta() const { return alerta_; }

  // true se um aviso deve ser enviado agora (e inicia o cooldown).
  bool deveEnviar(bool armado, uint32_t agora_ms, uint32_t cooldown_ms) {
    if (!alerta_ || !armado) return false;
    if (!enviou_ || (uint32_t)(agora_ms - ultimo_envio_) > cooldown_ms) {
      enviou_ = true;
      ultimo_envio_ = agora_ms;
      extras_msg_ = extras_;
      extras_ = 0;
      return true;
    }
    extras_++;
    return false;
  }

  // Picos registrados durante o cooldown anterior (para a mensagem do aviso).
  uint16_t extrasMsg() const { return extras_msg_; }

 private:
  bool flags_[FLAGS_N];
  int pos_ = 0;
  bool alerta_ = false;
  bool enviou_ = false;
  uint32_t ultimo_envio_ = 0;
  uint16_t extras_ = 0;
  uint16_t extras_msg_ = 0;
};

}  // namespace radar
