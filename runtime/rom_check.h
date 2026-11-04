/* Diagnostico do arquivo de ROM que o usuario forneceu.
 *
 * A ROM nao acompanha o executavel e nunca vai acompanhar: e' dado comercial
 * protegido, e o dump e' do usuario. Entao o programa VAI receber arquivo
 * errado — a pergunta nao e' se, e' o que ele faz quando receber.
 *
 * Ate' aqui md_load_rom aceitava qualquer arquivo de qualquer tamanho. Um dump
 * SMD, uma ROM truncada ou outro jogo bootavam em cima de lixo e davam tela
 * preta, e a tela preta nao diz nada a quem esta' do outro lado. Cada uma
 * dessas situacoes tem um conserto diferente, e o programa sabe qual e'.
 *
 * A ordem dos testes e' do mais estrutural para o mais especifico, porque e'
 * essa a ordem em que a mensagem fica util: nao adianta falar de checksum para
 * quem abriu um .zip.
 */

#ifndef TINYTOON_ROM_CHECK_H
#define TINYTOON_ROM_CHECK_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    ROM_OK = 0,             /* e' exatamente a ROM que o projeto valida */
    ROM_OUTRA_REVISAO,      /* e' o jogo certo, mas nao byte a byte a mesma */
    ROM_VAZIA,
    ROM_TAMANHO_ERRADO,
    ROM_SMD,                /* dump interleaved .smd em vez de .bin cru */
    ROM_NAO_E_MEGADRIVE,
    ROM_OUTRO_JOGO,
    ROM_CORROMPIDA          /* header certo, checksum interno nao fecha */
} RomVeredito;

/* Codigo de saida do programa quando a ROM e' recusada: SAIDA_ROM_BASE +
 * veredito. A ordem do enum acima e' portanto CONTRATO — o exit_codes do
 * recomplauncher.json traduz cada numero numa frase para o jogador:
 *
 *   11 outra revisao (so' no build com jogo.dll, que precisa da exata)
 *   12 vazia   13 tamanho errado   14 SMD   15 nao e' Mega Drive
 *   16 outro jogo   17 checksum errado
 *
 * Inserir um veredito no meio renumeraria os seguintes em silencio. Novos
 * vereditos vao no FIM do enum. */
#define SAIDA_ROM_BASE 10

/* Examina o arquivo carregado. Nao modifica nada. */
RomVeredito rom_verificar(const uint8_t *dados, size_t tamanho);

/* Frase curta para titulo de janela ou primeira linha de log. */
const char *rom_veredito_curto(RomVeredito v);

/* Explicacao com o que fazer a respeito. Varias linhas, para caixa de mensagem. */
const char *rom_veredito_longo(RomVeredito v);

/* SHA-1 de um bloco. Exposto porque o log imprime o hash do arquivo do usuario
 * quando ele nao bate — sem isso, "outra revisao" e' um beco sem saida: ninguem
 * consegue dizer QUAL revisao sem o numero. */
void sha1(const uint8_t *dados, size_t n, uint8_t saida[20]);

#endif /* TINYTOON_ROM_CHECK_H */
