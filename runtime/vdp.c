#include "vdp.h"

#include "memory.h"
#include "png.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t vram[VDP_VRAM_SIZE];
static uint16_t cram[VDP_CRAM_WORDS];
static uint16_t vsram[VDP_VSRAM_WORDS];
static uint8_t regs[VDP_REGS];

unsigned long vdp_vram_writes, vdp_cram_writes, vdp_vsram_writes;

/* MEDICAO: escritas no meio do quadro, que e' o que efeito raster significa.
 * Mudar paleta ou registrador durante o retraco e' rotina; durante o display
 * ativo muda a imagem linha a linha. */
unsigned long vdp_cram_meio_quadro, vdp_reg_meio_quadro, vdp_vsram_meio_quadro;
unsigned long vdp_dma_transfers, vdp_dma_bytes;
/* MEDICAO: o que o FIFO e o custo de acesso cobrariam.
 *
 * No hardware o 68000 PARA durante um DMA 68k->VRAM, e trava quando escreve na
 * porta de dados mais rapido do que o VDP drena. Hoje as duas coisas sao de
 * graca aqui. Antes de implementar, medir quanto isso custaria de verdade. */
unsigned long vdp_dma_palavras_68k, vdp_dma_palavras_fill, vdp_dma_palavras_copia;
unsigned long vdp_dados_display_ativo;

/* Bit 6 do status: o VDP desistiu de desenhar sprites em alguma linha.
 * Limpo na leitura, como no hardware. */
static int sprite_overflow;


/* Estado da porta de controle.
 *
 * A porta e' de 16 bits e o protocolo tem duas formas que se distinguem pelos
 * dois bits do topo da PRIMEIRA palavra: 10xxxxxx e' escrita de registrador e
 * termina ali; qualquer outra coisa e' a primeira metade de um endereco, e a
 * segunda palavra completa. Escrever 32 bits de uma vez (o jogo faz isso o
 * tempo todo, com o comando inteiro numa escrita long) e' so' as duas palavras em sequencia —
 * o barramento ja' divide em dois md_write16. */
static int segunda_palavra;     /* esperando a metade de baixo do endereco? */
static uint16_t codigo;         /* CD0-CD5: o que a porta de dados vai fazer */
static uint16_t endereco;       /* endereco corrente, 16 bits */

/* Preenchimento por DMA fica armado ate' a proxima escrita na porta de dados,
 * que e' quem traz o byte a repetir. */
static int dma_fill_armado;

static uint16_t auto_incremento(void) { return regs[0x0F]; }

/* Bit 5 do registrador $01: interrupcao de V-blank habilitada.
 *
 * Sem isso o handler roda desde o primeiro frame, antes de o boot inicializar
 * as sombras de registrador que ele usa — e um handler que le' sombra zerada
 * escreve lixo na porta de controle. O jogo so' liga esse bit quando esta'
 * pronto para ser interrompido, e essa e' exatamente a informacao que faltava. */
int vdp_vint_habilitada(void) { return (regs[0x01] & 0x20) != 0; }

static void avancar(void) {
    endereco = (uint16_t)(endereco + auto_incremento());
}

/* Estado interno, so' para leitura, para os mods.
 *
 * Devolvido como ponteiro const em vez de copiado: sao 128 KB por quadro se
 * fosse copia, e um mod que so' quer olhar duas palavras nao deve pagar isso.
 * O const e' o contrato; nada impede fisicamente um mod de escrever, e isso
 * esta' dito em mod.h. */
const uint8_t  *vdp_vram_ponteiro(void) { return vram; }
const uint16_t *vdp_cram_ponteiro(void) { return cram; }
const uint8_t  *vdp_regs_ponteiro(void) { return regs; }

void vdp_reset(void) {
    memset(vram, 0, sizeof vram);
    memset(cram, 0, sizeof cram);
    memset(vsram, 0, sizeof vsram);
    memset(regs, 0, sizeof regs);
    segunda_palavra = 0;
    codigo = 0;
    endereco = 0;
    dma_fill_armado = 0;
}

/* --- escrita nas memorias -------------------------------------------------
 *
 * A VRAM e' endereçada por byte mas escrita por palavra, e o bit 0 do endereco
 * TROCA os dois bytes em vez de deslocar. Nao e' curiosidade: e' o que faz o
 * preenchimento por DMA distribuir o byte do jeito que o jogo espera. */
static void gravar_vram_palavra(uint16_t addr, uint16_t valor) {
    if (addr & 1) {
        vram[(addr - 1) & 0xFFFF] = (uint8_t)valor;
        vram[addr & 0xFFFF] = (uint8_t)(valor >> 8);
    } else {
        vram[addr & 0xFFFF] = (uint8_t)(valor >> 8);
        vram[(addr + 1) & 0xFFFF] = (uint8_t)valor;
    }
    vdp_vram_writes++;
}

static void gravar_por_codigo(uint16_t valor) {
    switch (codigo & 0x0F) {
    case 0x1: gravar_vram_palavra(endereco, valor); break;
    case 0x3: cram[(endereco >> 1) & (VDP_CRAM_WORDS - 1)] = valor & 0x0EEE;
              vdp_cram_writes++;
              if (!md_em_vblank()) vdp_cram_meio_quadro++;
              break;
    case 0x5: { unsigned i = (endereco >> 1);
                if (i < VDP_VSRAM_WORDS) vsram[i] = valor & 0x07FF;
                vdp_vsram_writes++;
                if (!md_em_vblank()) vdp_vsram_meio_quadro++; } break;
    default: break;   /* codigo de leitura recebendo escrita: ignorado */
    }
    avancar();
}

/* O 68000 PARA durante o DMA — o VDP toma o barramento.
 *
 * As taxas sao do hardware em H40: o VDP drena ~83,5 palavras por linha durante
 * o retraco e ~9 com o display ativo, e uma linha vale 490 ciclos do 68000.
 * Dai 5,9 e 54 ciclos por palavra.
 *
 * Medido antes de implementar: isto custa 1.682 ciclos por quadro em media, 1%
 * do orcamento — pouco na media e concentrado nas transicoes de tela, que sao
 * exatamente os quadros em que o console engasga.
 *
 * O TRAVAMENTO POR FIFO CHEIO NAO ESTA' AQUI, e a razao e' medida: este jogo faz
 * 153 escritas na porta de dados com display ativo em 20.000 quadros — zero por
 * quadro. Sem escrita durante o display nao ha' como encher a fila de quatro
 * posicoes. Implementar a maquina de estado do FIFO seria codigo que nunca
 * executa. Se outro jogo entrar neste runtime, e' o primeiro lugar a olhar. */
static void cobrar_dma(uint32_t palavras) {
    md_cobrar_ciclos(md_em_vblank() ? (palavras * 59u / 10u) : (palavras * 54u));
}

/* --- DMA -----------------------------------------------------------------
 *
 * Tres modos, escolhidos pelos dois bits do topo do registrador $17:
 *   0x  68000 -> VDP    (o caminho normal: arte da ROM para a VRAM)
 *   10  preenchimento de VRAM com um byte
 *   11  copia dentro da VRAM
 *
 * O comprimento em $13/$14 conta PALAVRAS para os dois primeiros modos e bytes
 * para a copia. Zero nao transfere nada — ver a justificativa em disparar_dma,
 * que e' onde essa decisao e' tomada e onde ela pode estar errada. O endereco de
 * origem em $15/$16/$17 e' em palavras, por isso o deslocamento de 1 bit. */
static void dma_de_68k(void) {
    uint32_t origem = ((uint32_t)regs[0x15]
                     | ((uint32_t)regs[0x16] << 8)
                     | ((uint32_t)(regs[0x17] & 0x7F) << 16)) << 1;
    uint32_t n = (uint32_t)regs[0x13] | ((uint32_t)regs[0x14] << 8);
    if (n == 0) return;   /* ver disparar_dma */

    for (uint32_t i = 0; i < n; i++) {
        uint16_t valor = (uint16_t)((md_peek8(origem) << 8) | md_peek8(origem + 1));
        origem += 2;
        gravar_por_codigo(valor);
    }
    vdp_dma_transfers++;
    vdp_dma_bytes += n * 2;
    vdp_dma_palavras_68k += n;
    cobrar_dma(n);
}

static void dma_copia_vram(void) {
    uint16_t origem = (uint16_t)(regs[0x15] | (regs[0x16] << 8));
    uint32_t n = (uint32_t)regs[0x13] | ((uint32_t)regs[0x14] << 8);
    if (n == 0) return;   /* ver disparar_dma */

    for (uint32_t i = 0; i < n; i++) {
        vram[endereco & 0xFFFF] = vram[origem & 0xFFFF];
        origem++;
        avancar();
    }
    vdp_vram_writes += n;
    vdp_dma_transfers++;
    vdp_dma_bytes += n;
    vdp_dma_palavras_copia += n;
    cobrar_dma(n);
}

/* O preenchimento so' acontece na proxima escrita da porta de dados: e' de la'
 * que vem o byte. Ate' la' o VDP fica armado. */
static void dma_preenche(uint16_t valor) {
    uint32_t n = (uint32_t)regs[0x13] | ((uint32_t)regs[0x14] << 8);
    if (n == 0) return;   /* ver disparar_dma */

    /* A primeira palavra vai inteira; o resto e' so' o byte alto. */
    gravar_vram_palavra(endereco, valor);
    avancar();
    for (uint32_t i = 1; i < n; i++) {
        vram[endereco & 0xFFFF] = (uint8_t)(valor >> 8);
        avancar();
    }
    vdp_vram_writes += n;
    vdp_dma_transfers++;
    vdp_dma_bytes += n;
    vdp_dma_palavras_fill += n;
    cobrar_dma(n);
    dma_fill_armado = 0;
}

unsigned long vdp_dma_len_zero;


static void disparar_dma(void) {
    if (!(regs[0x01] & 0x10)) return;      /* DMA desabilitado no registrador 1 */
    /* COMPRIMENTO ZERO = NENHUMA TRANSFERENCIA.
     *
     * Isso contraria a convencao usual dos emuladores, que tratam 0 como 65536
     * (o contador de 16 bits decrementaria e daria a volta). A evidencia contra
     * ela vem do proprio jogo, e e' especifica:
     *
     * O handler de V-blank chama sub_001922 em $000450, que monta o DMA da
     * tabela de sprites lendo o comprimento de $FFFB3C. Quem escreve $FFFB3C e'
     * sub_0016A8 — chamada em $00047C, ou seja, DEPOIS. E sub_0016A8 tem
     * exatamente um chamador, esse. Logo, no primeiro V-blank o comprimento e'
     * inevitavelmente o valor de inicializacao ($94009300 = zero), no console
     * tanto quanto aqui.
     *
     * Com a convencao de 65536, esse primeiro DMA despeja 128 KB da RAM sobre a
     * VRAM inteira. O jogo nunca mais limpa a VRAM — os unicos DMAs para
     * endereco baixo depois do boot sao para CRAM (cod $23) — entao o tile 0
     * fica sujo para sempre, e o plano B, cuja tabela e' toda zeros, mostra esse
     * lixo na tela inteira. Um jogo lancado nao pode fazer isso no console.
     *
     * Portanto a inferencia: neste caminho, comprimento zero nao transfere.
     * E' inferencia de COMPORTAMENTO, nao de datasheet — se outra tela quebrar
     * de um jeito que so' 65536 explique, e' aqui que se olha primeiro. */
    if (!regs[0x13] && !regs[0x14]) { vdp_dma_len_zero++; return; }
    switch (regs[0x17] >> 6) {
    case 0: case 1: dma_de_68k(); break;
    case 2: dma_fill_armado = 1; break;    /* espera o byte na porta de dados */
    case 3: dma_copia_vram(); break;
    }
}

/* --- portas --------------------------------------------------------------- */

void vdp_write_control(uint16_t value) {
    if (!segunda_palavra) {
        if ((value & 0xC000) == 0x8000) {
            unsigned r = (value >> 8) & 0x1F;
            if (r < VDP_REGS) regs[r] = (uint8_t)value;
            if (!md_em_vblank()) vdp_reg_meio_quadro++;
            return;
        }
        codigo = (uint16_t)((codigo & 0x3C) | ((value >> 14) & 0x03));
        endereco = (uint16_t)((endereco & 0xC000) | (value & 0x3FFF));
        segunda_palavra = 1;
        return;
    }

    segunda_palavra = 0;
    codigo = (uint16_t)((codigo & 0x03) | ((value >> 2) & 0x3C));
    endereco = (uint16_t)((endereco & 0x3FFF) | ((value & 0x03) << 14));
    if (codigo & 0x20) disparar_dma();
}

void vdp_write_data(uint16_t value) {
    if (!md_em_vblank()) vdp_dados_display_ativo++;
    /* Uma escrita na porta de dados no meio de um endereco de duas palavras
     * cancela a segunda metade — o hardware faz isso, e o codigo do jogo conta
     * com o comportamento em alguns caminhos. */
    segunda_palavra = 0;

    if (dma_fill_armado) { dma_preenche(value); return; }
    gravar_por_codigo(value);
}

uint16_t vdp_read_data(void) {
    uint16_t valor = 0;
    segunda_palavra = 0;
    switch (codigo & 0x0F) {
    case 0x0: valor = (uint16_t)((vram[endereco & 0xFFFF] << 8)
                               | vram[(endereco + 1) & 0xFFFF]); break;
    case 0x8: valor = cram[(endereco >> 1) & (VDP_CRAM_WORDS - 1)]; break;
    case 0x4: { unsigned i = endereco >> 1;
                valor = i < VDP_VSRAM_WORDS ? vsram[i] : 0; } break;
    default: break;
    }
    avancar();
    return valor;
}

static int tela_altura(void);

/* Status do VDP, derivado do relogio.
 *
 * Ate' aqui o bit de V-blank ALTERNAVA a cada leitura — um stub que destravava
 * "espere comecar, espere terminar" sem emular tempo nenhum. Com o relogio em
 * ciclos, a linha de varredura sai de graca: 153.409 ciclos divididos por 313
 * linhas dao 490 ciclos por linha.
 *
 * A excecao importante: enquanto o HANDLER de V-blank roda, o V-blank esta'
 * acontecendo por construcao, e e' isso que se reporta. Sem essa regra a conta
 * daria "linha 0, display ativo" justamente durante o retraco, porque o handler
 * e' chamado quando o contador do quadro acabou de virar — e 71 das 77 leituras
 * que este jogo faz sao de dentro do handler.
 *
 * LER A PORTA DE CONTROLE ZERA O LATCH DE SEGUNDA PALAVRA. Isso nao e' detalhe:
 * sem ele o latch fica preso em 1 e a PROXIMA escrita de registrador do jogo e'
 * engolida como metade de um endereco. Foi assim que um "$96FC" (registrador
 * $16) virou palavra de comando com CD5 ligado e disparou um DMA fantasma de
 * 65535 palavras, sujando o tile 0. O sintoma aparecia como fundo de ruido
 * colorido, muito longe da causa.
 */
#define CICLOS_POR_LINHA 490u
#define LINHAS_POR_QUADRO 313u          /* PAL */
#define CICLOS_ATIVOS_LINHA 410u        /* o resto da linha e' H-blank */

unsigned long vdp_status_dentro, vdp_status_fora;

uint16_t vdp_read_status(void) {
    segunda_palavra = 0;
    if (md_em_vblank()) vdp_status_dentro++; else vdp_status_fora++;

    uint32_t c = md_ciclos_no_quadro();
    unsigned linha = c / CICLOS_POR_LINHA;
    unsigned na_linha = c % CICLOS_POR_LINHA;

    /* Bits 10, 12 e 13 sao lidos como 1 no hardware. Bit 0 e' PAL: esta ROM e'
     * regiao E, e o jogo usa isso para escolher constantes de tempo. */
    uint16_t st = 0x3400u | 0x0001u;
    if (md_em_vblank() || linha >= (unsigned)tela_altura()) st |= 0x0008u;
    if (na_linha >= CICLOS_ATIVOS_LINHA) st |= 0x0004u;
    if (md_frames & 1) st |= 0x0010u;   /* quadro impar */
    /* Bit 6: o VDP desistiu de desenhar sprites em alguma linha. O hardware
     * limpa na leitura, e por isso o jogo so' ve' cada estouro uma vez. */
    if (sprite_overflow) { st |= 0x0040u; sprite_overflow = 0; }
    return st;
}

/* Contador H/V em $C00008: linha no byte alto, posicao na linha no baixo.
 *
 * Medido: este jogo NUNCA le' este registrador (zero acessos em 20.000
 * quadros). Esta' aqui porque agora custa tres linhas, e porque devolver lixo
 * de um registrador que existe e' pior que devolver o valor certo. */
uint16_t vdp_read_hv(void) {
    uint32_t c = md_ciclos_no_quadro();
    unsigned linha = (c / CICLOS_POR_LINHA) % LINHAS_POR_QUADRO;
    unsigned na_linha = (c % CICLOS_POR_LINHA) * 256u / CICLOS_POR_LINHA;
    return (uint16_t)(((linha & 0xFF) << 8) | (na_linha & 0xFF));
}

/* --- conversao de cor -----------------------------------------------------
 *
 * CRAM guarda 0000 BBB0 GGG0 RRR0: tres bits por canal, oito niveis. Os niveis
 * nao sao lineares — sao os que o DAC do Mega Drive produz de fato. Usar
 * "nivel * 36" daria uma imagem parecida e errada nos tons baixos. */
static const uint8_t NIVEIS[8] = {0, 52, 87, 116, 144, 172, 206, 255};

static void cor_rgb(uint16_t c, uint8_t *out) {
    out[0] = NIVEIS[(c >> 1) & 7];   /* R */
    out[1] = NIVEIS[(c >> 5) & 7];   /* G */
    out[2] = NIVEIS[(c >> 9) & 7];   /* B */
}

/* --- geometria da tela, lida dos registradores ---------------------------- */

static int tela_largura(void) { return (regs[0x0C] & 0x81) ? 320 : 256; }
static int tela_altura(void)  { return (regs[0x01] & 0x08) ? 240 : 224; }

static void tamanho_plano(int *larg_tiles, int *alt_tiles) {
    static const int TAB[4] = {32, 64, 32, 128};   /* 10 e' invalido; vira 32 */
    *larg_tiles = TAB[regs[0x10] & 3];
    *alt_tiles = TAB[(regs[0x10] >> 4) & 3];
}

/* Pixel de um tile 4bpp: 32 bytes, 4 por linha, dois pixels por byte. */
static unsigned pixel_do_tile(unsigned tile, int x, int y) {
    unsigned off = (tile * 32u + (unsigned)y * 4u + (unsigned)(x >> 1)) & 0xFFFF;
    uint8_t b = vram[off];
    return (x & 1) ? (b & 0x0Fu) : (b >> 4);
}

/* --- despejo da VRAM como folha de tiles ---------------------------------- */

int vdp_dump_tiles(const char *path) {
    const int por_linha = 64;                 /* 64 tiles = 512 px de largura */
    const int total = VDP_VRAM_SIZE / 32;     /* 2048 tiles */
    const int linhas = total / por_linha;     /* 32 linhas = 256 px */
    const int W = por_linha * 8, H = linhas * 8;

    uint8_t *img = calloc((size_t)W * H * 3, 1);
    if (!img) return -1;

    for (int t = 0; t < total; t++) {
        int tx = (t % por_linha) * 8, ty = (t / por_linha) * 8;
        for (int y = 0; y < 8; y++) {
            for (int x = 0; x < 8; x++) {
                unsigned idx = pixel_do_tile((unsigned)t, x, y);
                uint8_t *p = img + (((size_t)(ty + y) * W) + (tx + x)) * 3;
                cor_rgb(cram[idx], p);
            }
        }
    }
    int r = png_write_rgb(path, img, W, H);
    free(img);
    return r;
}

/* --- despejo da CRAM ------------------------------------------------------ */

int vdp_dump_cram(const char *path) {
    const int celula = 24;
    const int W = 16 * celula, H = 4 * celula;
    uint8_t *img = calloc((size_t)W * H * 3, 1);
    if (!img) return -1;

    for (int pal = 0; pal < 4; pal++) {
        for (int c = 0; c < 16; c++) {
            uint8_t rgb[3];
            cor_rgb(cram[pal * 16 + c], rgb);
            for (int y = 0; y < celula; y++) {
                for (int x = 0; x < celula; x++) {
                    uint8_t *p = img + (((size_t)(pal * celula + y) * W)
                                        + (c * celula + x)) * 3;
                    p[0] = rgb[0]; p[1] = rgb[1]; p[2] = rgb[2];
                }
            }
        }
    }
    int r = png_write_rgb(path, img, W, H);
    free(img);
    return r;
}

/* --- composicao da tela ---------------------------------------------------
 *
 * Cada camada vira um buffer de "pixel do VDP": 0 e' transparente, senao
 * bit 15 = prioridade, bits 4-5 = paleta, bits 0-3 = indice de cor. Compor
 * depois na ordem certa e' mais simples — e mais facil de conferir — do que
 * decidir prioridade pixel a pixel durante o desenho. */
#define PX(prio, pal, idx) ((uint16_t)(0x4000u | ((prio) << 15) \
                                       | ((pal) << 4) | (idx)))
#define PX_IDX(v)  ((v) & 0x0Fu)
#define PX_PAL(v)  (((v) >> 4) & 3u)
#define PX_PRIO(v) (((v) >> 15) & 1u)

static uint16_t hscroll_de(int plano, int linha) {
    uint16_t base = (uint16_t)((regs[0x0D] & 0x3F) << 10);
    unsigned off;
    switch (regs[0x0B] & 3) {
    case 0: off = 0; break;                              /* tela inteira */
    case 1: off = (unsigned)(linha & 7) * 4u; break;     /* invalido no HW */
    case 2: off = (unsigned)(linha & ~7) * 4u; break;    /* por celula */
    default: off = (unsigned)linha * 4u; break;          /* por linha */
    }
    unsigned a = (base + off + (unsigned)(plano ? 2 : 0)) & 0xFFFF;
    return (uint16_t)(((vram[a] << 8) | vram[(a + 1) & 0xFFFF]) & 0x03FF);
}

static uint16_t vscroll_de(int plano, int x) {
    unsigned i = (regs[0x0B] & 4) ? (unsigned)((x >> 4) * 2) : 0u;
    i += (unsigned)(plano ? 1 : 0);
    return i < VDP_VSRAM_WORDS ? (uint16_t)(vsram[i] & 0x03FF) : 0;
}

static void desenhar_plano(uint16_t *buf, int W, int H, int plano) {
    uint16_t base = plano ? (uint16_t)((regs[0x04] & 0x07) << 13)
                          : (uint16_t)((regs[0x02] & 0x38) << 10);
    int lt, at;
    tamanho_plano(&lt, &at);
    int lp = lt * 8, ap = at * 8;

    for (int y = 0; y < H; y++) {
        uint16_t hs = hscroll_de(plano, y);
        for (int x = 0; x < W; x++) {
            uint16_t vs = vscroll_de(plano, x);
            int px = ((x - hs) % lp + lp) % lp;
            int py = ((y + vs) % ap + ap) % ap;

            unsigned e = (base + (unsigned)((py / 8) * lt + (px / 8)) * 2u) & 0xFFFF;
            uint16_t entrada = (uint16_t)((vram[e] << 8) | vram[(e + 1) & 0xFFFF]);

            int tx = px & 7, ty = py & 7;
            if (entrada & 0x0800) tx = 7 - tx;    /* espelho horizontal */
            if (entrada & 0x1000) ty = 7 - ty;    /* espelho vertical */

            unsigned idx = pixel_do_tile(entrada & 0x07FF, tx, ty);
            buf[(size_t)y * W + x] = idx ? PX((entrada >> 15) & 1,
                                              (entrada >> 13) & 3, idx) : 0;
        }
    }
}

/* MEDICAO: o hardware desiste de desenhar sprites depois de 20 por linha ou 320
 * pixels de sprite na linha (16 e 256 em H32). O port desenha todos. Antes de
 * implementar o corte, medir se o jogo chega perto. */
unsigned long vdp_pico_sprites_linha, vdp_pico_pixels_linha;
unsigned long vdp_linhas_estouradas;
unsigned long vdp_linhas_cortadas;


/* Ordem em que o hardware processa os sprites: a lista ligada, comecando no
 * indice 0 e seguindo o campo de link. NAO e' a ordem do endereco na tabela. */
typedef struct { int sy, sx, lc, ac; unsigned e; } SpriteInfo;

unsigned long vdp_mascaras, vdp_mascaras_visiveis;

static int montar_lista(SpriteInfo *lista, int maximo) {
    uint16_t base = (uint16_t)((regs[0x05] & 0x7F) << 9);
    unsigned atual = 0;
    int n = 0;
    while (n < maximo) {
        unsigned e = (base + atual * 8u) & 0xFFFF;
        unsigned tam = vram[(e + 2) & 0xFFFF];
        lista[n].e = e;
        lista[n].sy = (int)((((vram[e] << 8) | vram[e + 1]) & 0x03FF)) - 128;
        lista[n].sx = (int)((((vram[(e + 6) & 0xFFFF] << 8)
                            | vram[(e + 7) & 0xFFFF]) & 0x01FF)) - 128;
        lista[n].lc = (int)((tam >> 2) & 3) + 1;
        lista[n].ac = (int)(tam & 3) + 1;
        /* MEDICAO: mascara de sprite.
         *
         * No hardware, um sprite com X bruto = 0 que NAO seja o primeiro da
         * lista nao e' desenhado — ele apaga todos os sprites de prioridade
         * menor nas linhas que cobre. E o unico recurso do VDP que este port
         * ainda nao implementa, e implementar sem saber se o jogo usa seria
         * adivinhar trabalho. Este contador responde. */
        if (n > 0 && lista[n].sx == -128) {
            vdp_mascaras++;
            if (lista[n].sy > -128 && lista[n].sy < 240) vdp_mascaras_visiveis++;
        }
        n++;
        unsigned prox = vram[(e + 3) & 0xFFFF];
        if (prox == 0 || prox >= (unsigned)maximo) break;
        atual = prox;
    }
    return n;
}

/* Onde o hardware DESISTE em cada linha.
 *
 * O VDP tem um orcamento por linha — 20 sprites e 320 pixels em H40, 16 e 256
 * em H32 — e processa a lista em ordem ate' estourar. O que vem depois
 * simplesmente NAO E' DESENHADO naquela linha, e o bit 6 do status acende.
 *
 * Medido neste jogo: pico de 21 sprites e 384 pixels por linha, 208 linhas
 * estourando em 20.000 quadros. Raro, mas real — e onde acontece, o console
 * mostra menos sprites do que o port mostrava.
 *
 * Como o corte e' por ORDEM, basta guardar o indice do primeiro sprite barrado
 * em cada linha: quem vem antes passa, quem vem depois nao. */
static void calcular_corte(const SpriteInfo *lista, int n, int W, int H,
                           short *corte) {
    const unsigned lim_sprites = (W == 320) ? 20u : 16u;
    const unsigned lim_pixels = (unsigned)W;
    unsigned char contagem[256] = {0};
    unsigned short pixels[256] = {0};

    for (int l = 0; l < H && l < 256; l++) corte[l] = (short)n;

    for (int i = 0; i < n; i++) {
        for (int l = 0; l < lista[i].ac * 8; l++) {
            int dy = lista[i].sy + l;
            if (dy < 0 || dy >= H || dy >= 256) continue;
            if (corte[dy] < (short)i) continue;          /* ja' cortou aqui */
            if (contagem[dy] >= lim_sprites || pixels[dy] >= lim_pixels) {
                corte[dy] = (short)i;
                vdp_linhas_cortadas++;
                sprite_overflow = 1;
                continue;
            }
            contagem[dy]++;
            pixels[dy] = (unsigned short)(pixels[dy] + lista[i].lc * 8);
        }
    }

    /* Os picos SEM o corte, para saber o quanto o jogo pede alem do orcamento.
     * Medir aqui e' o que garante que a medicao e a regra sejam a mesma coisa —
     * duas implementacoes se separam, e a que ninguem olha e' a que erra. */
    unsigned char pedido[256] = {0};
    unsigned short px_pedido[256] = {0};
    for (int i = 0; i < n; i++) {
        for (int l = 0; l < lista[i].ac * 8; l++) {
            int dy = lista[i].sy + l;
            if (dy < 0 || dy >= H || dy >= 256) continue;
            pedido[dy]++;
            px_pedido[dy] = (unsigned short)(px_pedido[dy] + lista[i].lc * 8);
        }
    }
    for (int l = 0; l < H && l < 256; l++) {
        if (pedido[l] > vdp_pico_sprites_linha) vdp_pico_sprites_linha = pedido[l];
        if (px_pedido[l] > vdp_pico_pixels_linha) vdp_pico_pixels_linha = px_pedido[l];
        if (pedido[l] > lim_sprites || px_pedido[l] > lim_pixels) {
            vdp_linhas_estouradas++;
        }
    }
}

/* Percorre a lista de sprites e conta quantos, e quantos pixels, caem em cada
 * linha. Separado do desenho de proposito: no build headless os sprites so' sao
 * desenhados uma vez, ao sair, e a medicao precisa de todo quadro. */
void vdp_tick_quadro(void) {
    SpriteInfo lista[80];
    short corte[256];
    int W = tela_largura(), H = tela_altura();
    int n = montar_lista(lista, (W == 320) ? 80 : 64);
    calcular_corte(lista, n, W, H, corte);
}

static void desenhar_sprites(uint16_t *buf, int W, int H) {
    int maximo = (W == 320) ? 80 : 64;
    SpriteInfo lista[80];
    short corte[256];

    int n = montar_lista(lista, maximo);
    calcular_corte(lista, n, W, H, corte);

    for (int i = 0; i < n; i++) {
        unsigned e = lista[i].e;
        int sy = lista[i].sy, sx = lista[i].sx;
        int lc = lista[i].lc, ac = lista[i].ac;
        uint16_t attr = (uint16_t)((vram[(e + 4) & 0xFFFF] << 8)
                                 | vram[(e + 5) & 0xFFFF]);
        unsigned tile0 = attr & 0x07FF;
        unsigned pal = (attr >> 13) & 3, prio = (attr >> 15) & 1;
        int hflip = (attr >> 11) & 1, vflip = (attr >> 12) & 1;

        for (int cx = 0; cx < lc; cx++) {
            for (int cy = 0; cy < ac; cy++) {
                /* Os tiles de um sprite sao guardados em COLUNAS, nao em
                 * linhas: primeiro a coluna inteira, depois a proxima. */
                unsigned tile = tile0 + (unsigned)(cx * ac + cy);
                int ox = hflip ? (lc - 1 - cx) * 8 : cx * 8;
                int oy = vflip ? (ac - 1 - cy) * 8 : cy * 8;
                for (int y = 0; y < 8; y++) {
                    int dy = sy + oy + (vflip ? 7 - y : y);
                    if (dy < 0 || dy >= H) continue;
                    /* O orcamento da linha ja' acabou antes deste sprite. */
                    if (dy < 256 && corte[dy] <= (short)i) continue;
                    for (int x = 0; x < 8; x++) {
                        int dx = sx + ox + (hflip ? 7 - x : x);
                        if (dx < 0 || dx >= W) continue;
                        unsigned idx = pixel_do_tile(tile, x, y);
                        if (!idx) continue;
                        size_t o = (size_t)dy * W + dx;
                        if (buf[o]) continue;   /* o primeiro da lista ganha */
                        buf[o] = PX(prio, pal, idx);
                    }
                }
            }
        }
    }
}

void vdp_screen_size(int *w, int *h) { *w = tela_largura(); *h = tela_altura(); }

int vdp_render(uint8_t *img, int W, int H) {
    size_t n = (size_t)W * H;

    uint16_t *pa = calloc(n, sizeof *pa);
    uint16_t *pb = calloc(n, sizeof *pb);
    uint16_t *sp = calloc(n, sizeof *sp);
    if (!pa || !pb || !sp) {
        free(pa); free(pb); free(sp);
        return -1;
    }

    desenhar_plano(pa, W, H, 0);
    desenhar_plano(pb, W, H, 1);
    desenhar_sprites(sp, W, H);

    /* Ordem de composicao do VDP, de baixo para cima. Cada camada nao
     * transparente cobre a anterior. */
    uint16_t fundo = cram[regs[0x07] & 0x3F];
    for (size_t i = 0; i < n; i++) {
        uint16_t cor = fundo;
        uint16_t camadas[6] = {pb[i], pa[i], sp[i], pb[i], pa[i], sp[i]};
        int quer_prio[6] = {0, 0, 0, 1, 1, 1};
        for (int c = 0; c < 6; c++) {
            uint16_t v = camadas[c];
            if (!v || (int)PX_PRIO(v) != quer_prio[c]) continue;
            cor = cram[PX_PAL(v) * 16 + PX_IDX(v)];
        }
        cor_rgb(cor, img + i * 3);
    }

    free(pa); free(pb); free(sp);
    return 0;
}

int vdp_dump_screen(const char *path) {
    int W, H;
    vdp_screen_size(&W, &H);
    uint8_t *img = malloc((size_t)W * H * 3);
    if (!img) return -1;
    int r = vdp_render(img, W, H) == 0 ? png_write_rgb(path, img, W, H) : -1;
    free(img);
    return r;
}

/* VRAM crua, para analisar fora do runtime. Uma imagem diz que esta' errado;
 * os bytes dizem o que esta' la'. */
int vdp_dump_vram_bin(const char *path) {
    FILE *fh = fopen(path, "wb");
    if (!fh) return -1;
    size_t n = fwrite(vram, 1, sizeof vram, fh);
    return (fclose(fh) == 0 && n == sizeof vram) ? 0 : -1;
}

/* Cada camada sozinha, com transparente em magenta.
 *
 * Compor tres camadas e olhar o resultado diz que ALGO esta' errado; nao diz
 * qual. Separar custa quinze linhas e transforma "o fundo esta' bugado" em "o
 * plano B esta' desenhando onde deveria ser transparente". */
int vdp_dump_layers(const char *prefixo) {
    int W, H;
    vdp_screen_size(&W, &H);
    size_t n = (size_t)W * H;
    uint16_t *buf = calloc(n, sizeof *buf);
    uint8_t *img = malloc(n * 3);
    if (!buf || !img) { free(buf); free(img); return -1; }

    static const char *NOMES[3] = {"-planoA.png", "-planoB.png", "-sprites.png"};
    int erro = 0;
    for (int c = 0; c < 3; c++) {
        memset(buf, 0, n * sizeof *buf);
        if (c == 2) desenhar_sprites(buf, W, H);
        else desenhar_plano(buf, W, H, c);

        for (size_t i = 0; i < n; i++) {
            if (!buf[i]) {   /* magenta = transparente, cor que o jogo nao usa */
                img[i * 3] = 255; img[i * 3 + 1] = 0; img[i * 3 + 2] = 255;
            } else {
                cor_rgb(cram[PX_PAL(buf[i]) * 16 + PX_IDX(buf[i])], img + i * 3);
            }
        }
        char caminho[256];
        snprintf(caminho, sizeof caminho, "%s%s", prefixo, NOMES[c]);
        if (png_write_rgb(caminho, img, W, H) != 0) erro = 1;
    }
    free(buf); free(img);
    return erro ? -1 : 0;
}

void vdp_report(void) {
    int lt, at;
    tamanho_plano(&lt, &at);
    printf("VDP: %dx%d, plano %dx%d tiles, display %s\n",
           tela_largura(), tela_altura(), lt, at,
           (regs[0x01] & 0x40) ? "ligado" : "DESLIGADO");
    printf("  plano A $%04X | plano B $%04X | sprites $%04X | hscroll $%04X\n",
           (regs[0x02] & 0x38) << 10, (regs[0x04] & 0x07) << 13,
           (regs[0x05] & 0x7F) << 9, (regs[0x0D] & 0x3F) << 10);
    printf("  escritas: VRAM %lu, CRAM %lu, VSRAM %lu | DMA %lu (%lu bytes)\n",
           vdp_vram_writes, vdp_cram_writes, vdp_vsram_writes,
           vdp_dma_transfers, vdp_dma_bytes);
    printf("  janela $%04X, corte H=$%02X V=$%02X | scroll H modo %d, V modo %d\n",
           (regs[0x03] & 0x3E) << 10, regs[0x11], regs[0x12],
           regs[0x0B] & 3, (regs[0x0B] >> 2) & 1);
    printf("  DMAs de comprimento zero: %lu\n", vdp_dma_len_zero);
    if (md_frames) {
        /* Custo em ciclos do 68000, pelas taxas do hardware em H40:
         * DMA 68k->VRAM drena ~83,5 palavras por linha durante o retraco, e uma
         * linha vale 490 ciclos -> ~5,9 ciclos por palavra. Preenchimento e
         * copia usam a mesma janela. */
        unsigned long long ciclos_dma =
            (unsigned long long)(vdp_dma_palavras_68k + vdp_dma_palavras_fill
                                 + vdp_dma_palavras_copia) * 59 / 10;
        printf("  custo que o FIFO/DMA cobrariam: %llu ciclos por quadro "
               "(%llu%% de %u)\n",
               ciclos_dma / md_frames, ciclos_dma * 100 / md_frames / 153409u,
               153409u);
        printf("  escritas na porta de dados com display ATIVO: %lu "
               "(%lu por quadro)\n",
               vdp_dados_display_ativo, vdp_dados_display_ativo / md_frames);
    }
    printf("  sprites por linha: pico %lu (limite %d), pixels pico %lu "
           "(limite %d), linhas pedindo demais %lu, cortes %lu\n",
           vdp_pico_sprites_linha, tela_largura() == 320 ? 20 : 16,
           vdp_pico_pixels_linha, tela_largura(), vdp_linhas_estouradas,
           vdp_linhas_cortadas);
    printf("  mascaras de sprite (X=0 fora da primeira posicao): %lu, "
           "delas na area visivel %lu\n", vdp_mascaras, vdp_mascaras_visiveis);
    printf("  leituras de status: %lu dentro do handler, %lu fora\n",
           vdp_status_dentro, vdp_status_fora);
    printf("  no MEIO do quadro (raster): CRAM %lu, registradores %lu, VSRAM %lu\n",
           vdp_cram_meio_quadro, vdp_reg_meio_quadro, vdp_vsram_meio_quadro);
    printf("  regs:");
    for (int i = 0; i < VDP_REGS; i++) printf(" %02X", regs[i]);
    printf("\n");
}
