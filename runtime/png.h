/* Gravador de PNG minimo, sem dependencia externa.
 *
 * Existe para o runtime poder despejar VRAM, CRAM e tela sem arrastar zlib nem
 * libpng para dentro do projeto — o container de build tem so' gcc. O truque e'
 * que o formato deflate permite blocos *armazenados* (sem compressao), entao um
 * PNG valido sai de um zlib stream trivial. Arquivo maior, zero dependencia.
 */

#ifndef TINYTOON_PNG_H
#define TINYTOON_PNG_H

#include <stdint.h>

/* Grava RGB de 8 bits por canal, 3 bytes por pixel, sem alpha.
 * Devolve 0 em sucesso. */
int png_write_rgb(const char *path, const uint8_t *rgb, int width, int height);

#endif /* TINYTOON_PNG_H */
