/* Saida de audio do runtime.
 *
 * A REGRA QUE ORGANIZA TUDO AQUI: o som e' gerado JUNTO COM O RELOGIO, amostra
 * por amostra, e nao em lote no fim do quadro.
 *
 * A primeira versao gerava as 882 amostras de uma vez em host_frame(). Nesse
 * instante o registrador $2A — a amostra do DAC — tem UM valor, o ultimo que o
 * Z80 escreveu. As 882 amostras do quadro recebiam todas esse mesmo valor,
 * quando deveriam ser ~880 valores distintos. Todo PCM virava ruido, e a FM so'
 * enxergava o estado dos registradores na fronteira do quadro.
 *
 * Agora audio_gerar() e' chamado do mesmo ponto que faz o Z80 andar, e as
 * amostras vao para um anel que o hospedeiro drena. Som e emulacao passam a ver
 * o mesmo tempo.
 */

#ifndef TINYTOON_AUDIO_H
#define TINYTOON_AUDIO_H

#include <stdint.h>

/* Relogios, todos derivados do mestre de 53.693.175 Hz:
 *
 *   /7   7.670.454 Hz   68000 e YM2612  (o YM recebe o MESMO clock da CPU)
 *   /15  3.579.545 Hz   Z80 e PSG
 *
 * O YM divide internamente por 144, o que da' 53.267 amostras por segundo.
 * Errar este numero por um fator de 7 nao produz som errado: produz musica no
 * andamento errado, porque os dois temporizadores contam nessa base. */
#define MD_CLOCK_Z80  3579545u
#define MD_CLOCK_YM   7670454u
#define YM_DIV        144u

/* A taxa NATIVA do chip. Gerar nela e deixar o SDL converter para o dispositivo
 * evita um estagio de reamostragem meu — que era por retencao, o metodo mais
 * grosseiro que existe. */
#define MD_TAXA_AUDIO (MD_CLOCK_YM / YM_DIV)     /* 53.267 Hz */

void audio_reset(void);

/* Pares que um quadro rende. Numero FIXO: a 50 Hz e 53.267 Hz sao 1065.
 *
 * Amarrar a producao a` contagem de acessos a memoria — que e' estimativa e
 * varia de quadro a quadro — gerava 6% de amostras a mais e enchia a fila do
 * dispositivo. Quando ela enche, um quadro inteiro e' descartado no meio da
 * forma de onda, e descartar amostra no meio de uma onda e' literalmente um
 * gerador de clique. O total por quadro tem que ser exato. */
#define MD_PARES_QUADRO (MD_TAXA_AUDIO / 50u)

/* A TAXA QUE O PORT REALMENTE PRODUZ, e que e' a que o dispositivo tem que
 * receber.
 *
 * 53.267 / 50 nao e' inteiro: sao 1065,34. Produzindo 1065 por quadro, a saida
 * real e' 1065 * 50 = 53.250 amostras por segundo, e nao 53.267.
 *
 * Enquanto o dispositivo era aberto a 53.267, essa diferenca de 17 amostras por
 * segundo drenava a fila devagar ate' ela SECAR — uma falha a cada ~63 segundos,
 * que e' um chiado raro e sem padrao aparente. Custou tempo justamente por ser
 * raro: parecia defeito de sintese, e era divisao inteira.
 *
 * Abrir o dispositivo na taxa que de fato produzimos zera a deriva: um quadro de
 * jogo passa a valer exatamente 1/50 de segundo de audio, para sempre. O preco
 * e' 0,03% de erro de afinacao contra o chip real — 0,006 de um semitom, uma
 * ordem de grandeza abaixo do que se ouve. */
#define MD_TAXA_SAIDA (MD_PARES_QUADRO * 50u)    /* 53.250 Hz */

/* Produz exatamente ``pares`` amostras. Chamado do relogio, com a quantidade
 * proporcional ao quanto do quadro ja' passou — e' isso que mantem o DAC
 * amostrado no instante certo sem deixar a taxa flutuar. */
void audio_gerar(unsigned pares);

/* Retira ate' ``quadros`` pares (esquerda, direita) do anel. Devolve quantos
 * pares de fato saiu. */
unsigned audio_drenar(int16_t *destino, unsigned quadros);

/* Quantos pares estao esperando. */
unsigned audio_disponivel(void);

extern unsigned long audio_estourou;   /* anel cheio: o hospedeiro nao drenou */

#endif /* TINYTOON_AUDIO_H */
