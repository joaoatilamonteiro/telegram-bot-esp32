// Gerado por exportar_modelo_h.py (modelo versao: 1). Não editar à mão.
#pragma once
#include <stdint.h>

#define ARV_N 15
#define FEAT_OSC_P90 1
#define FEAT_ENERGIA 4
#define ARV_N_FEATURES 5

static const int16_t ARV_ESQ[ARV_N] = {1, 2, 3, -1, -1, 6, -1, -1, 9, 10, -1, -1, 13, -1, -1};
static const int16_t ARV_DIR[ARV_N] = {8, 5, 4, -1, -1, 7, -1, -1, 12, 11, -1, -1, 14, -1, -1};
static const int16_t ARV_FEAT[ARV_N] = {1, 1, 1, -2, -2, 4, -2, -2, 1, 4, -2, -2, 4, -2, -2};
static const float ARV_LIM[ARV_N] = {1.41401172f, 1.28792548f, 1.21326447f, -2.0f, -2.0f, 1.29666555f, -2.0f, -2.0f, 1.58850145f, 1.71700621f, -2.0f, -2.0f, 3.07850122f, -2.0f, -2.0f};
static const float ARV_PROB[ARV_N] = {0.5f, 0.0128820976f, 0.00214843568f, 0.000425909966f, 0.0212175995f, 0.187961742f, 0.635565817f, 0.099247463f, 0.95730859f, 0.675943613f, 0.91016978f, 0.372162133f, 0.97791642f, 0.984256148f, 0.907282293f};
