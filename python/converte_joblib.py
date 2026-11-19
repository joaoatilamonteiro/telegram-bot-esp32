#código para converter arquivos de arvore.joblib para arvore.h para usar na esp32

import sys
from pathlib import Path
import joblib
import numpy as np

USADAS_PELO_FIRMWARE = {"osc_p90", "energia"}


def fmt_float(v):
    s = f"{float(v):.9g}"
    if not any(c in s for c in ".eE"):
        s+=".0"
    return s+"f" #que porra de código maluco é esse? o que caralhos essa funcao ta buscando??

def vetor(nome, tipo, valores, fmt):
    itens = ", ".join(fmt(v) for v in valores)
    return f"static const {tipo} {nome}[ARV_N] = {{{itens}}};\n"


def percurso(x, esq, dir_, feat, lim, prob):
    """Mesmo laço que o firmware vai executar."""
    n = 0
    while esq[n] != -1:
        n = esq[n] if x[feat[n]] <= lim[n] else dir_[n]
    return prob[n]


def exportar(caminho_modelo, caminho_h):
    pacote = joblib.load(caminho_modelo)
    modelo, nomes = pacote["modelo"], list(pacote["features"])
    t = modelo.tree_

    usadas = {nomes[i] for i in set(t.feature.tolist()) if i >= 0}
    if not usadas <= USADAS_PELO_FIRMWARE:
        sys.exit(f"A árvore usa features que a ESP não calcula: {usadas - USADAS_PELO_FIRMWARE}")

    esq = t.children_left.astype(int)
    dir_ = t.children_right.astype(int)
    feat = t.feature.astype(int)
    lim = t.threshold.astype(np.float32)
    prob = (t.value[:, 0, 1] / t.value[:, 0, :].sum(axis=1)).astype(np.float32)

    # validação: o percurso exportado tem que bater com o sklearn
    rng = np.random.default_rng(0)
    X = rng.uniform(0.0, 4.0, size=(1000, len(nomes))).astype(np.float32)
    X[:, nomes.index("osc_p90")] = rng.uniform(0.5, 3.0, 1000)
    X[:, nomes.index("energia")] = rng.uniform(0.5, 5.0, 1000)
    esperado = modelo.predict_proba(X)[:, 1]
    obtido = np.array([percurso(x, esq, dir_, feat, lim, prob) for x in X])
    erro = float(np.abs(esperado - obtido).max())
    if erro > 1e-6:
        sys.exit(f"Exportação diverge do sklearn (erro máximo {erro:.3g})")

    linhas = [
        f"// Gerado por exportar_modelo_h.py (modelo versao: {pacote.get('versao', '?')}). Não editar à mão.\n",
        "#pragma once\n#include <stdint.h>\n\n",
        f"#define ARV_N {t.node_count}\n",
    ]
    for nome in ("osc_p90", "energia"):
        linhas.append(f"#define FEAT_{nome.upper()} {nomes.index(nome)}\n")
    linhas.append(f"#define ARV_N_FEATURES {len(nomes)}\n\n")
    linhas.append(vetor("ARV_ESQ", "int16_t", esq, lambda v: str(int(v))))
    linhas.append(vetor("ARV_DIR", "int16_t", dir_, lambda v: str(int(v))))
    linhas.append(vetor("ARV_FEAT", "int16_t", feat, lambda v: str(int(v))))
    linhas.append(vetor("ARV_LIM", "float", lim, fmt_float))
    linhas.append(vetor("ARV_PROB", "float", prob, fmt_float))

    Path(caminho_h).write_text("".join(linhas), encoding="utf-8")
    print(f"OK: {t.node_count} nós, erro máximo vs sklearn = {erro:.2e}, gravado em {caminho_h}")


if __name__ == "__main__":
    raiz = Path(__file__).resolve().parent.parent
    entrada = sys.argv[1] if len(sys.argv) > 1 else raiz / "python" / "modelo_arvore.joblib"
    saida = sys.argv[2] if len(sys.argv) > 2 else raiz / "src" / "modelo_arvore.h"
    exportar(entrada, saida)