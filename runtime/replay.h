/* Gravacao e reproducao de input.
 *
 * Um "replay" e' a lista das mudancas de botao, com o quadro em que cada uma
 * aconteceu. Como o port e' deterministico — mesmo input, mesmo estado, sempre —
 * reproduzir o script leva o jogo exatamente ao mesmo lugar.
 *
 * Isso resolve o problema que sobrou depois do mapa de leitura: o explorador
 * aleatorio recomeca do zero toda vez e joga fora tudo o que descobriu. Com
 * replay, uma sequencia que chegou longe vira PREFIXO, e o aleatorio so' age
 * depois que o script acaba. E' assim que a maquina joga fundo sem ninguem.
 *
 * Grava por md_pad_set, entao nao importa quem apertou: teclado, gamepad,
 * explorador ou o proprio replay. Uma partida de verdade grava igual, e vira
 * material permanente em vez de vinte minutos que ninguem repete.
 *
 * Formato de texto de propósito: da' para ler, cortar e versionar.
 *
 *     # tinytoon replay v1
 *     <quadro> <porta> <botoes em hex>
 */

#ifndef TINYTOON_REPLAY_H
#define TINYTOON_REPLAY_H

/* Carrega um script. Devolve o numero de eventos, ou -1 se nao abriu. */
int md_replay_carregar(const char *path);

/* Ultimo quadro do script carregado; 0 se nao ha' script. O explorador so' age
 * depois deste quadro — senao os dois disputariam o mesmo controle. */
unsigned long md_replay_ultimo(void);

/* Aplica as mudancas marcadas para este quadro. */
void md_replay_quadro(unsigned long quadro);

/* Liga a gravacao. Tudo que passar por md_pad_set daqui em diante entra. */
void md_replay_gravar_em(const char *path);

/* Chamado por md_pad_set. Ignora repeticao do mesmo valor. */
void md_replay_registrar(unsigned porta, unsigned botoes);

/* Grava o arquivo. Zero se deu certo, -1 se falhou ou se nao havia gravacao. */
int md_replay_fechar(void);

#endif /* TINYTOON_REPLAY_H */
