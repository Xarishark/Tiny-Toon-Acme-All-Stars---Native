/* Camada de apresentacao: janela, entrada e ritmo.
 *
 * E' o unico lugar do projeto que conhece SDL. A logica recompilada fala com o
 * VDP e com o joypad atraves do barramento; o VDP entrega um frame em RGB; e
 * este arquivo o coloca numa janela. Trocar SDL por outra coisa — ou renderizar
 * em resolucao nativa em vez de 320x224 — e' mexer so' aqui.
 *
 * Sem TINYTOON_SDL definido, tudo vira no-op e o programa roda headless,
 * despejando PNG ao sair. E' assim que ele roda numa maquina sem tela.
 */

#ifndef TINYTOON_HOST_H
#define TINYTOON_HOST_H

/* Devolve 0 em sucesso. Sem SDL, sempre 0. */
int host_init(void);

/* Um frame entregue pelo V-blank: desenha, apresenta, le' entrada e espera o
 * tempo que faltar. Chamado de dentro do laco do jogo. */
void host_frame(void);

void host_shutdown(void);

/* Contadores da saida de audio: fila, descartes e quadros incompletos.
 * No build headless nao ha' dispositivo, entao nao imprime nada. */
void audio_relatorio(void);

#endif /* TINYTOON_HOST_H */
