#include "input.h"

#include "memory.h"
#include "replay.h"

static unsigned botoes[MD_PORTAS];   /* 1 = pressionado */
static uint8_t dados[MD_PORTAS];     /* ultimo byte escrito na porta de dados */
static uint8_t ctrl[MD_PORTAS];      /* direcao: 1 = saida */

void md_pad_set(unsigned porta, unsigned b) {
    if (porta >= MD_PORTAS) return;
    /* A gravacao mora aqui, e nao em quem aperta, de proposito: teclado,
     * gamepad, explorador e o proprio replay passam todos por este ponto. Uma
     * partida de verdade grava com o mesmo codigo que uma corrida de cobertura. */
    md_replay_registrar(porta, b);
    botoes[porta] = b;
}

void md_pad_write(unsigned porta, uint8_t valor) {
    if (porta < MD_PORTAS) dados[porta] = valor;
}

void md_pad_set_ctrl(unsigned porta, uint8_t valor) {
    if (porta < MD_PORTAS) ctrl[porta] = valor;
}

uint8_t md_pad_read(unsigned porta) {
    if (porta >= MD_PORTAS) return 0xFF;

    unsigned b = botoes[porta];
    /* TH e' pino de saida quando o jogo o configura como tal; senao flutua em
     * 1. Ler a linha de volta e' o que o jogo usa para saber em que metade
     * esta'. */
    int th = (ctrl[porta] & 0x40) ? ((dados[porta] & 0x40) != 0) : 1;

    uint8_t v;
    if (th) {
        v = (uint8_t)(0x40
            | (!(b & MD_PAD_UP)    << 0)
            | (!(b & MD_PAD_DOWN)  << 1)
            | (!(b & MD_PAD_LEFT)  << 2)
            | (!(b & MD_PAD_RIGHT) << 3)
            | (!(b & MD_PAD_B)     << 4)
            | (!(b & MD_PAD_C)     << 5));
    } else {
        /* Com TH em zero, esquerda e direita leem zero — e' assim que o jogo
         * distingue "controle presente" de porta vazia. */
        v = (uint8_t)(0x00
            | (!(b & MD_PAD_UP)    << 0)
            | (!(b & MD_PAD_DOWN)  << 1)
            | (!(b & MD_PAD_A)     << 4)
            | (!(b & MD_PAD_START) << 5));
    }

    /* Pinos configurados como saida leem de volta o que foi escrito, nao o
     * controle. Sem isso o jogo pode nao reconhecer a propria escrita de TH. */
    uint8_t saida = ctrl[porta];
    return (uint8_t)((v & ~saida) | (dados[porta] & saida));
}

/* ------------------------------------------------- explorador de cobertura */

/* Aperta botoes sozinho, para o jogo visitar telas que o modo atracao nunca
 * mostra.
 *
 * O mapa de leitura da ROM so' classifica o que o jogo LE, e o jogo so' le' o
 * que a partida em curso pede. Rodando so' a atracao, arte de personagem que
 * ninguem escolheu e dado de fase que ninguem entrou continuam opacos para
 * sempre. Alguem tem que jogar — e uma maquina joga mais barato que uma pessoa.
 *
 * Nao ha' nada de inteligente aqui: e' um macaco em teclado. Isso basta porque
 * o objetivo nao e' ganhar a partida, e' ATRAVESSAR menu e chegar em tela nova.
 * START tem peso maior justamente por isso: quase todo menu deste jogo abre com
 * START, e um sorteio uniforme perderia tempo demais parado na primeira tela.
 *
 * Deterministico de proposito. Uma corrida de cobertura precisa ser repetivel
 * para valer como evidencia: mesma semente, mesmos botoes, mesmo mapa. E rodar
 * varias sementes em paralelo cobre mais do que uma corrida longa, porque cada
 * uma toma um caminho diferente na arvore de menus. */
static uint32_t semente;
static int explorando;
static unsigned segurando[MD_PORTAS];
static unsigned resta[MD_PORTAS];

static uint32_t xorshift(void) {
    semente ^= semente << 13;
    semente ^= semente >> 17;
    semente ^= semente << 5;
    return semente;
}

void md_explorar_ligar(uint32_t s) {
    semente = s ? s : 1u;
    explorando = 1;
}

int md_explorando(void) { return explorando; }

void md_explorar_quadro(void) {
    static const unsigned SORTEIO[16] = {
        MD_PAD_START, MD_PAD_START, MD_PAD_START, MD_PAD_A,
        MD_PAD_B, MD_PAD_C, MD_PAD_UP, MD_PAD_DOWN,
        MD_PAD_LEFT, MD_PAD_RIGHT, MD_PAD_UP | MD_PAD_B,
        MD_PAD_DOWN | MD_PAD_C, MD_PAD_LEFT | MD_PAD_A,
        MD_PAD_RIGHT | MD_PAD_B, MD_PAD_A | MD_PAD_B | MD_PAD_C, 0u,
    };
    if (!explorando) return;
    /* Enquanto o script do replay manda, o explorador cala a boca. Os dois
     * escrevendo no mesmo controle dariam um terceiro comportamento, que nao e'
     * nem o gravado nem o sorteado — e ai' o replay deixaria de reproduzir. */
    if (md_frames <= md_replay_ultimo()) return;
    for (unsigned p = 0; p < MD_PORTAS; p++) {
        if (resta[p]) { resta[p]--; continue; }
        uint32_t r = xorshift();
        /* Solta o botao metade das vezes: menu que le' borda de subida ignora
         * botao que nunca sobe, e o explorador ficaria preso na mesma tela. */
        segurando[p] = (r & 1) ? 0u : SORTEIO[(r >> 8) & 15];
        resta[p] = 3u + ((r >> 16) % 14u);
        md_pad_set(p, segurando[p]);
    }
}
