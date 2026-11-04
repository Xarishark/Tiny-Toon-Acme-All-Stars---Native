/* Lado do runtime do sistema de mods.
 *
 * O contrato que os autores de mod leem esta' em runtime/mod.h. Este header e'
 * interno: so' o que host.c e main.c precisam chamar.
 */

#ifndef TINYTOON_MODS_H
#define TINYTOON_MODS_H

#include "mod.h"

/* Varre `dir` por bibliotecas compartilhadas e carrega as que se identificarem.
 * Diretorio ausente nao e' erro: a maioria das sessoes nao tem mod nenhum. */
void mods_carregar(const char *dir);

/* Uma vez por quadro de jogo, antes de compor a imagem. */
void mods_quadro(void);

/* Com a imagem pronta, antes de ir para a tela. Chamado uma vez por quadro
 * APRESENTADO, que no avanco rapido e' bem menos que um por quadro de jogo. */
void mods_pixels(unsigned char *rgb, int largura, int altura);

/* Quantos carregaram. Zero e' o caso normal. */
int mods_ativos(void);

void mods_encerrar(void);

#endif /* TINYTOON_MODS_H */
