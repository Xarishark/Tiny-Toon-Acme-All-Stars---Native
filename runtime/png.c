#include "png.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t crc_tabela[256];
static int crc_pronta;

static void montar_crc(void) {
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) {
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        crc_tabela[n] = c;
    }
    crc_pronta = 1;
}

static uint32_t crc32_de(const uint8_t *dados, size_t n, uint32_t inicial) {
    if (!crc_pronta) montar_crc();
    uint32_t c = inicial ^ 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c = crc_tabela[(c ^ dados[i]) & 0xFF] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

static uint32_t adler32_de(const uint8_t *dados, size_t n) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; i++) {
        a = (a + dados[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

static void por_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

/* Um chunk PNG: tamanho, tipo, dados, CRC sobre tipo+dados. */
static int gravar_chunk(FILE *fh, const char *tipo,
                        const uint8_t *dados, size_t n) {
    uint8_t cab[8];
    por_be32(cab, (uint32_t)n);
    memcpy(cab + 4, tipo, 4);
    if (fwrite(cab, 1, 8, fh) != 8) return -1;
    if (n && fwrite(dados, 1, n, fh) != n) return -1;

    uint32_t crc = crc32_de((const uint8_t *)tipo, 4, 0);
    if (n) crc = crc32_de(dados, n, crc);
    uint8_t fim[4];
    por_be32(fim, crc);
    return fwrite(fim, 1, 4, fh) == 4 ? 0 : -1;
}

int png_write_rgb(const char *path, const uint8_t *rgb, int width, int height) {
    if (width <= 0 || height <= 0) return -1;

    /* Dados brutos do PNG: cada linha comeca com o byte de filtro (0 = nenhum). */
    size_t linha = (size_t)width * 3 + 1;
    size_t bruto_n = linha * (size_t)height;
    uint8_t *bruto = malloc(bruto_n);
    if (!bruto) return -1;
    for (int y = 0; y < height; y++) {
        uint8_t *dst = bruto + linha * (size_t)y;
        dst[0] = 0;
        memcpy(dst + 1, rgb + (size_t)y * (size_t)width * 3, (size_t)width * 3);
    }

    /* Stream zlib com blocos deflate armazenados. Cada bloco leva 5 bytes de
     * cabecalho e ate' 65535 de dados. */
    size_t blocos = (bruto_n + 65534) / 65535;
    if (blocos == 0) blocos = 1;
    size_t z_n = 2 + blocos * 5 + bruto_n + 4;
    uint8_t *z = malloc(z_n);
    if (!z) { free(bruto); return -1; }

    size_t p = 0;
    z[p++] = 0x78;      /* CMF: deflate, janela de 32K */
    z[p++] = 0x01;      /* FLG: sem dicionario, compressao minima */

    size_t restante = bruto_n, off = 0;
    do {
        size_t pedaco = restante > 65535 ? 65535 : restante;
        z[p++] = (uint8_t)((restante - pedaco == 0) ? 1 : 0);  /* BFINAL, BTYPE=00 */
        z[p++] = (uint8_t)pedaco;
        z[p++] = (uint8_t)(pedaco >> 8);
        z[p++] = (uint8_t)~(uint8_t)pedaco;
        z[p++] = (uint8_t)~(uint8_t)(pedaco >> 8);
        memcpy(z + p, bruto + off, pedaco);
        p += pedaco; off += pedaco; restante -= pedaco;
    } while (restante);

    por_be32(z + p, adler32_de(bruto, bruto_n));
    p += 4;
    free(bruto);

    FILE *fh = fopen(path, "wb");
    if (!fh) { free(z); return -1; }

    static const uint8_t assinatura[8] = {137, 'P', 'N', 'G', 13, 10, 26, 10};
    int erro = fwrite(assinatura, 1, 8, fh) != 8;

    uint8_t ihdr[13];
    por_be32(ihdr, (uint32_t)width);
    por_be32(ihdr + 4, (uint32_t)height);
    ihdr[8] = 8;    /* bits por canal */
    ihdr[9] = 2;    /* tipo de cor: RGB */
    ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    erro |= gravar_chunk(fh, "IHDR", ihdr, sizeof ihdr);
    erro |= gravar_chunk(fh, "IDAT", z, p);
    erro |= gravar_chunk(fh, "IEND", NULL, 0);

    free(z);
    if (fclose(fh) != 0) erro = 1;
    return erro ? -1 : 0;
}
