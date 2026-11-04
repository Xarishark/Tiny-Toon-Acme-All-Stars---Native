#include "replay.h"

#include "input.h"
#include "memory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { unsigned long quadro; unsigned char porta; unsigned short botoes; } Evento;

/* Teto generoso. Uma hora de jogo a 50 Hz com uma mudanca a cada tres quadros
 * da' ~60.000 eventos; isto cabe dez vezes isso. Estourar nao corrompe nada,
 * so' para de gravar e avisa uma vez — perder o fim de uma gravacao e' ruim,
 * mas escrever fora do vetor seria pior. */
#define MAX_EVENTOS 600000

static Evento *script;              /* o que reproduzir */
static int script_n, script_i;
static unsigned long script_ultimo;

static Evento *gravacao;            /* o que esta' sendo gravado */
static int gravacao_n, gravacao_cheia;
static const char *gravacao_caminho;
static unsigned gravacao_estado[MD_PORTAS];
static int gravacao_iniciada;

int md_replay_carregar(const char *path) {
    FILE *fh = fopen(path, "r");
    if (!fh) return -1;

    script = calloc(MAX_EVENTOS, sizeof *script);
    if (!script) { fclose(fh); return -1; }

    char linha[128];
    script_n = 0;
    while (fgets(linha, sizeof linha, fh) && script_n < MAX_EVENTOS) {
        if (linha[0] == '#' || linha[0] == '\n') continue;
        unsigned long q; unsigned p, b;
        if (sscanf(linha, "%lu %u %x", &q, &p, &b) != 3) continue;
        if (p >= MD_PORTAS) continue;
        script[script_n].quadro = q;
        script[script_n].porta = (unsigned char)p;
        script[script_n].botoes = (unsigned short)b;
        script_n++;
    }
    fclose(fh);

    /* O script tem que estar em ordem de quadro — md_replay_quadro consome em
     * sequencia. Um arquivo editado a mao fora de ordem perderia eventos em
     * silencio, entao ordena aqui em vez de confiar. */
    for (int i = 1; i < script_n; i++) {
        Evento e = script[i];
        int j = i - 1;
        while (j >= 0 && script[j].quadro > e.quadro) { script[j + 1] = script[j]; j--; }
        script[j + 1] = e;
    }
    script_ultimo = script_n ? script[script_n - 1].quadro : 0;
    script_i = 0;
    return script_n;
}

unsigned long md_replay_ultimo(void) { return script_ultimo; }

void md_replay_quadro(unsigned long quadro) {
    while (script_i < script_n && script[script_i].quadro <= quadro) {
        md_pad_set(script[script_i].porta, script[script_i].botoes);
        script_i++;
    }
}

void md_replay_gravar_em(const char *path) {
    gravacao = calloc(MAX_EVENTOS, sizeof *gravacao);
    if (!gravacao) return;
    gravacao_caminho = path;
    gravacao_n = 0;
    gravacao_iniciada = 1;
    for (unsigned p = 0; p < MD_PORTAS; p++) gravacao_estado[p] = 0;
}

void md_replay_registrar(unsigned porta, unsigned botoes) {
    if (!gravacao_iniciada || porta >= MD_PORTAS) return;
    if (gravacao_estado[porta] == botoes) return;   /* nada mudou */
    gravacao_estado[porta] = botoes;
    if (gravacao_n >= MAX_EVENTOS) {
        if (!gravacao_cheia) {
            gravacao_cheia = 1;
            fprintf(stdout, "[replay] gravacao cheia em %d eventos; o resto "
                            "da sessao nao entra\n", MAX_EVENTOS);
        }
        return;
    }
    gravacao[gravacao_n].quadro = md_frames;
    gravacao[gravacao_n].porta = (unsigned char)porta;
    gravacao[gravacao_n].botoes = (unsigned short)botoes;
    gravacao_n++;
}

int md_replay_fechar(void) {
    if (!gravacao_iniciada || !gravacao_caminho) return -1;
    FILE *fh = fopen(gravacao_caminho, "w");
    if (!fh) return -1;
    fprintf(fh, "# tinytoon replay v1 - %d eventos\n", gravacao_n);
    for (int i = 0; i < gravacao_n; i++) {
        fprintf(fh, "%lu %u %04X\n", gravacao[i].quadro,
                gravacao[i].porta, gravacao[i].botoes);
    }
    return fclose(fh) == 0 ? 0 : -1;
}
