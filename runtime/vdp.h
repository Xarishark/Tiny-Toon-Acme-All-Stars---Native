/* VDP do Mega Drive — armazenamento e leitura de tela.
 *
 * Escopo desta versao: o VDP **guarda** o que recebe (VRAM, CRAM, VSRAM,
 * registradores, DMA) e sabe transformar isso em imagem sob demanda. O que ele
 * ainda NAO faz e' temporizacao: nao ha' contador de linha, nao ha' FIFO, nao
 * ha' custo de acesso. O bit de V-blank do status continua alternando a cada
 * leitura — ver vdp_read_status.
 *
 * Essa separacao e' deliberada. Guardar corretamente e' o que permite ver o que
 * o jogo esta' desenhando; temporizar corretamente e' o que permite efeitos por
 * linha (raster) e vem depois. Misturar as duas coisas era o caminho mais curto
 * para nao conseguir depurar nenhuma das duas.
 */

#ifndef TINYTOON_VDP_H
#define TINYTOON_VDP_H

#include <stdint.h>

#define VDP_VRAM_SIZE   0x10000
#define VDP_CRAM_WORDS  64
#define VDP_VSRAM_WORDS 40
#define VDP_REGS        24

void vdp_reset(void);

/* Portas vistas pelo 68000 em $C00000 (dados) e $C00004 (controle). */
void vdp_write_control(uint16_t value);
void vdp_write_data(uint16_t value);
uint16_t vdp_read_data(void);
uint16_t vdp_read_status(void);

/* Contador H/V em $C00008: linha no byte alto, posicao na linha no baixo.
 * Medido: este jogo nunca le'. */
uint16_t vdp_read_hv(void);

/* Tamanho corrente da tela: 320x224 ou 256x224, lido dos registradores. O jogo
 * troca de modo entre telas (o logo da Konami e' H32), entao isso muda em
 * tempo de execucao e a janela precisa acompanhar. */
void vdp_screen_size(int *w, int *h);

/* Estado interno para leitura (mods). Ver runtime/mod.h. */
const uint8_t  *vdp_vram_ponteiro(void);
const uint16_t *vdp_cram_ponteiro(void);
const uint8_t  *vdp_regs_ponteiro(void);

/* Bit 5 do registrador $01. O V-blank so' pode ser entregue quando o jogo
 * habilita a interrupcao — antes disso o handler leria estado nao inicializado. */
int vdp_vint_habilitada(void);

/* Desenha o frame em RGB (3 bytes por pixel). O buffer e' do chamador.
 * Devolve 0 em sucesso. */
int vdp_render(uint8_t *rgb, int w, int h);

/* Despejos de diagnostico. Devolvem 0 em sucesso. */
int vdp_dump_tiles(const char *path);   /* VRAM inteira como folha de tiles */
int vdp_dump_cram(const char *path);    /* as 4 paletas, 16 cores cada */
int vdp_dump_screen(const char *path);  /* planos B+A e sprites, compostos */
int vdp_dump_layers(const char *prefixo); /* cada camada sozinha, para achar o culpado */
int vdp_dump_vram_bin(const char *path); /* VRAM crua, 64 KB */

/* Resumo do estado: modo de tela, bases de tabela, tamanho dos planos. */
/* Uma vez por quadro: mede sprites e pixels por linha contra o limite do
 * hardware. Separado do desenho porque o build headless nao desenha por quadro. */
void vdp_tick_quadro(void);

void vdp_report(void);

/* Quanto de cada memoria foi de fato escrito. Zero em VRAM significa que o
 * jogo nunca mandou tile nenhum — diagnostico, nao estatistica. */
extern unsigned long vdp_vram_writes, vdp_cram_writes, vdp_vsram_writes;
extern unsigned long vdp_dma_transfers, vdp_dma_bytes;
extern unsigned long vdp_dma_len_zero;

#endif /* TINYTOON_VDP_H */
