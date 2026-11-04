/* O que separa um binario de diagnostico de um programa que alguem usa.
 *
 * O build do dia a dia despeja tudo no terminal e le' `rom.bin` do diretorio
 * corrente. Isso e' certo para depurar e errado para distribuir: quem clica
 * duas vezes num .exe nao tem terminal, nao esta' no diretorio certo, e nao vai
 * ler nada que apareca e suma.
 *
 * Entao o build de release (TINYTOON_RELEASE) muda quatro coisas:
 *
 *   - subsistema de janela, sem console preto atras do jogo
 *   - o diagnostico vai para tinytoon.log AO LADO DO EXE, nao para o vazio
 *   - erro fatal vira caixa de mensagem, porque printf num app de janela
 *     e' o mesmo que silencio
 *   - a ROM e' procurada ao lado do exe; se nao estiver la', um seletor de
 *     arquivo abre em vez de o programa desistir
 *
 * Fora do release, tudo isto vira no-op e o comportamento nao muda em nada.
 */

#ifndef TINYTOON_RELEASE_H
#define TINYTOON_RELEASE_H

/* Prepara log e diretorio de trabalho. Chamar antes de qualquer printf. */
void release_iniciar(void);

/* Mostra o erro de um jeito que o usuario veja. Fora do release, imprime. */
void release_erro(const char *titulo, const char *corpo);

/* Onde esta' a ROM.
 *
 * Ordem: o argumento da linha de comando, depois `rom.bin` ao lado do exe,
 * depois um seletor de arquivo. Devolve NULL se o usuario cancelar.
 * O ponteiro e' estatico e vale ate' a proxima chamada.
 */
const char *release_achar_rom(const char *argumento);

/* Se este binario e' o de release. Usado para nao despejar PNG de diagnostico
 * no diretorio de quem so' quer jogar. */
int release_ativo(void);

/* Onde procurar mods: `mods/` AO LADO DO EXECUTAVEL, e nao do diretorio
 * corrente. Os dois divergem em atalho, "abrir com" e arrastar-e-soltar, e um
 * mod que so' carrega quando a pessoa abre pelo terminal e' pior que nenhum. */
const char *release_dir_mods(void);

#endif /* TINYTOON_RELEASE_H */
