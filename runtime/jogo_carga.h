/* Carrega a jogo.dll (jogo.so) e, se ela faltar ou for de outra ROM, roda o
 * montador para gera-la. So' existe no build com TINYTOON_JOGO_DLL — o de
 * distribuicao. Ver jogo_api.h para o porque da separacao.
 */

#ifndef TINYTOON_JOGO_CARGA_H
#define TINYTOON_JOGO_CARGA_H

#include "jogo_api.h"

/* Codigos de saida do programa quando o jogo nao pode ser montado ou
 * carregado. Contrato com o exit_codes do recomplauncher.json. */
#define SAIDA_JOGO_NAO_MONTADO 20   /* o montador falhou ou nao existe */
#define SAIDA_JOGO_INVALIDO    21   /* a DLL existe mas nao e' desta versao */

/* Garante um jogo carregado para a ROM de SHA-1 `rom_sha1` (40 hex).
 * Monta se preciso. Devolve 0 e preenche `saida`, ou um SAIDA_JOGO_*. */
int jogo_preparar(const char *caminho_rom, const char *rom_sha1,
                  JogoExporta *saida);

#endif /* TINYTOON_JOGO_CARGA_H */
