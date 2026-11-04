#include "rom_check.h"
#include "rom_baseline.h"

#include <string.h>

/* --------------------------------------------------------------- SHA-1 */

/* Implementado aqui em vez de puxar dependencia: sao sessenta linhas, e a
 * alternativa seria uma biblioteca inteira no executavel para calcular um hash
 * uma vez por execucao. */
static uint32_t rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

void sha1(const uint8_t *dados, size_t n, uint8_t saida[20]) {
    uint32_t h[5] = { 0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u,
                      0xC3D2E1F0u };
    uint64_t bits = (uint64_t)n * 8u;
    size_t total = n + 1;
    while (total % 64 != 56) total++;
    total += 8;

    uint8_t bloco[64];
    for (size_t base = 0; base < total; base += 64) {
        for (int i = 0; i < 64; i++) {
            size_t p = base + (size_t)i;
            if (p < n)              bloco[i] = dados[p];
            else if (p == n)        bloco[i] = 0x80;
            else if (p < total - 8) bloco[i] = 0x00;
            else                    bloco[i] = (uint8_t)(bits >> (8 * (total - 1 - p)));
        }
        uint32_t w[80];
        for (int i = 0; i < 16; i++) {
            w[i] = ((uint32_t)bloco[i * 4] << 24) | ((uint32_t)bloco[i * 4 + 1] << 16)
                 | ((uint32_t)bloco[i * 4 + 2] << 8) | (uint32_t)bloco[i * 4 + 3];
        }
        for (int i = 16; i < 80; i++)
            w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20)      { f = (b & c) | (~b & d);            k = 0x5A827999u; }
            else if (i < 40) { f = b ^ c ^ d;                     k = 0x6ED9EBA1u; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d);   k = 0x8F1BBCDCu; }
            else             { f = b ^ c ^ d;                     k = 0xCA62C1D6u; }
            uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    for (int i = 0; i < 5; i++) {
        saida[i * 4]     = (uint8_t)(h[i] >> 24);
        saida[i * 4 + 1] = (uint8_t)(h[i] >> 16);
        saida[i * 4 + 2] = (uint8_t)(h[i] >> 8);
        saida[i * 4 + 3] = (uint8_t)h[i];
    }
}

/* ---------------------------------------------------------- diagnostico */

/* A soma do header cobre de $200 ate' o fim, em palavras big-endian, com
 * transbordo descartado. E' a mesma conta de tools/rom_info.py --check; se as
 * duas discordarem, uma esta' errada. */
static uint16_t checksum(const uint8_t *d, size_t n) {
    uint32_t soma = 0;
    for (size_t i = 0x200; i + 1 < n; i += 2)
        soma += ((uint32_t)d[i] << 8) | d[i + 1];
    return (uint16_t)soma;
}

RomVeredito rom_verificar(const uint8_t *dados, size_t tamanho) {
    if (!dados || tamanho == 0) return ROM_VAZIA;

    /* Dump SMD antes de qualquer outra coisa: ele tem um cabecalho de 512 bytes
     * e os bytes intercalados, entao NENHUM outro teste faz sentido nele — a
     * string "SEGA" nem aparece contigua. E' o erro mais comum de quem baixou a
     * ROM em vez de dumpar, e tem conserto trivial. */
    if (tamanho % 16384 == 512) return ROM_SMD;

    if (tamanho < 0x200) return ROM_TAMANHO_ERRADO;
    if (memcmp(dados + 0x100, ROM_CONSOLE, strlen(ROM_CONSOLE)) != 0)
        return ROM_NAO_E_MEGADRIVE;

    /* O titulo overseas fica em $150, 48 bytes preenchidos com espaco. */
    if (memcmp(dados + 0x150, ROM_TITULO, strlen(ROM_TITULO)) != 0)
        return ROM_OUTRO_JOGO;

    if (tamanho != ROM_TAMANHO) return ROM_TAMANHO_ERRADO;

    uint16_t guardado = (uint16_t)(((uint32_t)dados[0x18E] << 8) | dados[0x18F]);
    if (guardado != checksum(dados, tamanho)) return ROM_CORROMPIDA;

    uint8_t h[20];
    sha1(dados, tamanho, h);
    if (memcmp(h, ROM_SHA1, 20) != 0) return ROM_OUTRA_REVISAO;

    return ROM_OK;
}

const char *rom_veredito_curto(RomVeredito v) {
    switch (v) {
    case ROM_OK:              return "ROM verified";
    case ROM_OUTRA_REVISAO:   return "Right game, different version of the ROM";
    case ROM_VAZIA:           return "The ROM file is empty";
    case ROM_TAMANHO_ERRADO:  return "The ROM file is the wrong size";
    case ROM_SMD:             return "The ROM is in SMD (interleaved) format";
    case ROM_NAO_E_MEGADRIVE: return "This file is not a Mega Drive ROM";
    case ROM_OUTRO_JOGO:      return "This ROM is a different game";
    case ROM_CORROMPIDA:      return "The ROM's internal checksum is wrong";
    }
    return "ROM in an unknown state";
}

/* Textos que o jogador le': em ingles, porque o programa e' distribuido para
 * fora. Os mesmos vereditos aparecem resumidos no exit_codes do
 * recomplauncher.json — mudar um aqui pede mudar o outro la'. */
const char *rom_veredito_longo(RomVeredito v) {
    switch (v) {
    case ROM_OK:
        return "The ROM matches, byte for byte, the one this port was built "
               "to run.";
    case ROM_OUTRA_REVISAO:
#ifdef TINYTOON_JOGO_DLL
        /* No build de distribuicao o jogo e' MONTADO desta ROM, e o
         * recompilador so' conhece a revisao analisada: outra revisao daria
         * codigo com enderecos errados. Ver main.c. */
        return "The header and checksum match, so this is Tiny Toon Adventures: "
               "ACME All-Stars, but not the European release this port was "
               "made from.\n\n"
               "The game is built from your ROM, and that only works with the "
               "exact European cartridge (GM T-95146-00, CRC32 1227b2b2). "
               "Another region, revision or a patched ROM cannot be used.\n\n"
               "The expected SHA-1 and yours are in tinytoon.log.";
#else
        return "The header and checksum match, so this is Tiny Toon Adventures: "
               "ACME All-Stars, but the file is not identical to the one this "
               "port is verified against.\n\n"
               "It may be another region, another revision or a patched ROM. "
               "The game will run, and probably well, but nothing here was "
               "tested against this file.\n\n"
               "The expected SHA-1 and yours are in tinytoon.log.";
#endif
    case ROM_VAZIA:
        return "The file is empty or could not be read.";
    case ROM_TAMANHO_ERRADO:
        return "The ROM must be exactly 1,048,576 bytes (1 MB).\n\n"
               "A smaller file is usually an interrupted download; a larger one "
               "is usually a .zip or .7z that has not been extracted yet.";
    case ROM_SMD:
        return "This is an SMD dump, with a 512-byte header and interleaved "
               "bytes. This port reads the raw BIN format.\n\n"
               "Convert it to .bin (any Mega Drive ROM tool can do it) and try "
               "again.";
    case ROM_NAO_E_MEGADRIVE:
        return "There is no \"SEGA MEGA DRIVE\" mark in the header, at "
               "$100.\n\n"
               "The file may be from another console, a compressed archive, or "
               "not a ROM at all.";
    case ROM_OUTRO_JOGO:
        return "This is a valid Mega Drive ROM, but of another game.\n\n"
               "This program is Tiny Toon Adventures: ACME All-Stars and runs "
               "only that game. It is not an emulator: it is the game itself, "
               "recompiled.";
    case ROM_CORROMPIDA:
        return "The header says this is the right game, but the checksum stored "
               "in it does not match the file's contents.\n\n"
               "The file is damaged or has been modified.";
    }
    return "Unexpected state.";
}
