#pragma once
// Calibracao e features do radar CSI. Porta de python/csi_features.py + Detector
// de python/demo_ao_vivo_web.py, so com o que o modelo_arvore.h usa (osc_p90 e energia).
// C++ puro (sem Arduino/ESP-IDF), para poder ser testado no PC.
#include <stdint.h>
#include <string.h>
#include <math.h>

namespace radar {

static const int N_SUB = 64;                    // subportadoras do LLTF
static const int MIN_PKTS = 20;                 // minimo de pacotes numa janela
static const uint32_t JANELA_MS = 1000;
static const uint32_t PASSO_MS = 500;
static const uint32_t CALIB_MS = 60000;         // duracao total da calibracao
static const uint32_t MASCARA_MS = 5000;        // parte inicial usada so para a mascara
static const int MIN_JANELAS_CALIB = 15;
static const int MAX_PKTS = 128;                // capacidade da janela (potencia de 2)
static const int MAX_JANELAS_CALIB = 160;

enum Fase : uint8_t { CALIBRANDO_MASCARA = 0, CALIBRANDO_JANELAS = 1, AO_VIVO = 2 };

struct Resultado {
  uint32_t ts;
  float osc_p90;
  float energia;
  uint16_t n_pacotes;
  float z[N_SUB];          // oscilacao relativa por subportadora (0 nas descartadas)
};

class Detector {
 public:
  Detector() { reiniciar(); }

  // Volta ao inicio da calibracao. Deve ser chamado na mesma task que chama adicionar().
  void reiniciar() {
    fase_ = CALIBRANDO_MASCARA;
    iniciou_ = false;
    t_ini_ = 0;
    ultimo_ts_ = 0;
    ini_ = 0;
    n_ = 0;
    tem_calc_ = false;
    ultimo_calc_ = 0;
    memset(soma_col_, 0, sizeof(soma_col_));
    n_mascara_ = 0;
    memset(soma_s_, 0, sizeof(soma_s_));
    n_jan_ = 0;
    memset(mask_, 0, sizeof(mask_));
    memset(idx_, 0, sizeof(idx_));
    K_ = 0;
    memset(sd_, 0, sizeof(sd_));
    en_base_ = 1.0f;
    memset(&ultimo_, 0, sizeof(ultimo_));
  }

  // Entrada de uma amostra: ts em ms, amp com N_SUB amplitudes.
  // Retorna true quando acabou de calcular uma nova janela AO VIVO (ver ultimo()).
  bool adicionar(uint32_t ts, const float *amp) {
    if (!amp) return false;

    // 1. entra na janela deslizante
    if (n_ == MAX_PKTS) {
      ini_ = (ini_ + 1) & (MAX_PKTS - 1);
      n_--;
    }
    int pos = (ini_ + n_) & (MAX_PKTS - 1);
    memcpy(amp_[pos], amp, sizeof(float) * N_SUB);
    ts_[pos] = ts;
    n_++;

    // 2. saem os pacotes com 1 s ou mais
    while (n_ > 0 && (uint32_t)(ts - ts_[ini_]) >= JANELA_MS) {
      ini_ = (ini_ + 1) & (MAX_PKTS - 1);
      n_--;
    }

    if (!iniciou_) {
      iniciou_ = true;
      t_ini_ = ts;
    }
    ultimo_ts_ = ts;
    uint32_t dec = ts - t_ini_;

    if (fase_ == CALIBRANDO_MASCARA) {
      for (int k = 0; k < N_SUB; k++) soma_col_[k] += amp[k];
      n_mascara_++;
      if (dec >= MASCARA_MS) finalizarMascara();
      return false;
    }

    if (fase_ == CALIBRANDO_JANELAS) {
      if (dec >= CALIB_MS) {
        finalizarCalibracao();
        return false;
      }
      if (passoDevido(ts)) {
        ultimo_calc_ = ts;
        tem_calc_ = true;
        if (n_ >= MIN_PKTS && n_jan_ < MAX_JANELAS_CALIB) {
          float s[N_SUB], en;
          medidas(s, &en);
          for (int j = 0; j < K_; j++) soma_s_[j] += s[j];
          en_calib_[n_jan_++] = en;
        }
      }
      return false;
    }

    // AO_VIVO
    if (passoDevido(ts)) {
      ultimo_calc_ = ts;
      tem_calc_ = true;
      if (n_ >= MIN_PKTS) {
        calculaResultado(ts);
        return true;
      }
    }
    return false;
  }

  // Define a base de fora (usado nos testes). mask/sd com N_SUB posicoes, en = base.en.
  void definirBase(const bool *mask, const float *sd, float en) {
    K_ = 0;
    for (int k = 0; k < N_SUB; k++) {
      mask_[k] = mask[k];
      sd_[k] = sd[k];
      if (mask[k]) idx_[K_++] = (uint8_t)k;
    }
    en_base_ = en;
    fase_ = AO_VIVO;
    iniciou_ = true;
  }

  Fase fase() const { return fase_; }
  const Resultado &ultimo() const { return ultimo_; }
  int pacotesNaJanela() const { return n_; }
  int subportadorasUsadas() const { return K_; }
  int janelasCalibracao() const { return n_jan_; }
  uint16_t falhasCalibracao() const { return falhas_; }
  const bool *mascara() const { return mask_; }
  const float *sdBase() const { return sd_; }
  float enBase() const { return en_base_; }

  float progresso() const {
    if (fase_ == AO_VIVO) return 1.0f;
    if (!iniciou_) return 0.0f;
    float p = (float)(uint32_t)(ultimo_ts_ - t_ini_) / (float)CALIB_MS;
    return p > 1.0f ? 1.0f : p;
  }

 private:
  bool passoDevido(uint32_t ts) const {
    return !tem_calc_ || (uint32_t)(ts - ultimo_calc_) >= PASSO_MS;
  }

  void falhaCalibracao() {
    uint16_t f = falhas_ + 1;
    reiniciar();
    falhas_ = f;
  }

  // Mascara = subportadoras com amplitude media > 20% da media geral (como cf.calibrar).
  void finalizarMascara() {
    float col[N_SUB];
    float tot = 0;
    for (int k = 0; k < N_SUB; k++) {
      col[k] = soma_col_[k] / (float)n_mascara_;
      tot += col[k];
    }
    tot /= (float)N_SUB;
    K_ = 0;
    for (int k = 0; k < N_SUB; k++) {
      mask_[k] = col[k] > 0.2f * tot;
      if (mask_[k]) idx_[K_++] = (uint8_t)k;
    }
    if (K_ < 8) {
      falhaCalibracao();
      return;
    }
    fase_ = CALIBRANDO_JANELAS;
    tem_calc_ = false;
  }

  // base.sd = media do desvio por subportadora; base.en = mediana da energia.
  void finalizarCalibracao() {
    if (n_jan_ < MIN_JANELAS_CALIB) {
      falhaCalibracao();
      return;
    }
    for (int j = 0; j < K_; j++) {
      float v = soma_s_[j] / (float)n_jan_;
      sd_[idx_[j]] = v > 1e-6f ? v : 1e-6f;
    }
    float v[MAX_JANELAS_CALIB];
    memcpy(v, en_calib_, sizeof(float) * n_jan_);
    ordena(v, n_jan_);
    float med = (n_jan_ % 2) ? v[n_jan_ / 2] : 0.5f * (v[n_jan_ / 2 - 1] + v[n_jan_ / 2]);
    en_base_ = med > 1e-6f ? med : 1e-6f;
    fase_ = AO_VIVO;
    tem_calc_ = false;
  }

  static void ordena(float *a, int n) {
    for (int i = 1; i < n; i++) {
      float x = a[i];
      int j = i - 1;
      while (j >= 0 && a[j] > x) {
        a[j + 1] = a[j];
        j--;
      }
      a[j + 1] = x;
    }
  }

  // Desvio (populacional) por subportadora de amp/media_do_pacote, e variacao de energia.
  // s[j] corresponde a idx_[j].
  void medidas(float *s, float *en) const {
    float media[MAX_PKTS];
    float x[MAX_PKTS];
    float soma_m = 0;
    for (int i = 0; i < n_; i++) {
      const float *p = amp_[(ini_ + i) & (MAX_PKTS - 1)];
      float t = 0;
      for (int j = 0; j < K_; j++) t += p[idx_[j]];
      media[i] = t / (float)K_;
      soma_m += media[i];
    }
    for (int j = 0; j < K_; j++) {
      int k = idx_[j];
      float m = 0;
      for (int i = 0; i < n_; i++) {
        float d = media[i] > 1e-9f ? media[i] : 1e-9f;
        x[i] = amp_[(ini_ + i) & (MAX_PKTS - 1)][k] / d;
        m += x[i];
      }
      m /= (float)n_;
      float v = 0;
      for (int i = 0; i < n_; i++) {
        float dd = x[i] - m;
        v += dd * dd;
      }
      s[j] = sqrtf(v / (float)n_);
    }
    float mm = soma_m / (float)n_;
    float v = 0;
    for (int i = 0; i < n_; i++) {
      float dd = media[i] - mm;
      v += dd * dd;
    }
    *en = sqrtf(v / (float)n_) / (mm > 1e-9f ? mm : 1e-9f);
  }

  void calculaResultado(uint32_t ts) {
    float s[N_SUB], en;
    medidas(s, &en);

    float ord[N_SUB];
    memset(ultimo_.z, 0, sizeof(ultimo_.z));
    for (int j = 0; j < K_; j++) {
      float z = s[j] / sd_[idx_[j]];
      ord[j] = z;
      ultimo_.z[idx_[j]] = z;
    }
    ordena(ord, K_);

    // percentil 90 com interpolacao linear (igual ao np.percentile)
    double pos = 0.9 * (K_ - 1);
    int lo = (int)pos;
    int hi = (lo + 1 < K_) ? lo + 1 : lo;
    float frac = (float)(pos - lo);

    ultimo_.ts = ts;
    ultimo_.osc_p90 = ord[lo] + (ord[hi] - ord[lo]) * frac;
    ultimo_.energia = en / en_base_;
    ultimo_.n_pacotes = (uint16_t)n_;
  }

  Fase fase_;
  bool iniciou_;
  uint32_t t_ini_, ultimo_ts_;

  // janela deslizante (ultimo 1 s)
  float amp_[MAX_PKTS][N_SUB];
  uint32_t ts_[MAX_PKTS];
  int ini_, n_;
  bool tem_calc_;
  uint32_t ultimo_calc_;

  // calibracao
  float soma_col_[N_SUB];
  int n_mascara_;
  float soma_s_[N_SUB];
  float en_calib_[MAX_JANELAS_CALIB];
  int n_jan_;
  uint16_t falhas_ = 0;

  // base
  bool mask_[N_SUB];
  uint8_t idx_[N_SUB];
  int K_;
  float sd_[N_SUB];
  float en_base_;

  Resultado ultimo_;
};

}  // namespace radar