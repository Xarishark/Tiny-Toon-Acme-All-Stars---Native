/* Configuracao do jogador: tinytoon.toml, ao lado do executavel.
 *
 * Existe para o RecompLauncher (recomplauncher.json, nivel 2): o launcher
 * edita este arquivo antes de cada partida e o programa le' na partida. Quem
 * nao usa o launcher pode editar a mao, ou nem saber que ele existe — sem o
 * arquivo, tudo fica no padrao.
 *
 * So' APRESENTACAO. Nenhuma chave aqui chega a logica do jogo: modo de imagem,
 * janela, qual controle e' qual porta, e onde esta' a ROM. Isso e' o que mantem
 * a regra de ouro — o jogo se comporta igual com qualquer combinacao.
 *
 * TOML PLANO, e nao TOML: `chave = valor`, uma por linha, `#` comenta. Nao ha'
 * tabelas nem listas porque nada aqui precisa delas, e um leitor completo de
 * TOML seria a maior parte deste modulo para nada.
 *
 * O arquivo NUNCA vai no zip da release: o launcher descompacta cada
 * atualizacao por cima da pasta, entao um tinytoon.toml embalado apagaria as
 * escolhas do jogador a cada versao nova. Os padroes vivem no codigo.
 */

#ifndef TINYTOON_CONFIG_H
#define TINYTOON_CONFIG_H

#define CONFIG_ARQUIVO "tinytoon.toml"

/* Le' o arquivo, se existir. Arquivo ausente nao e' erro. Chamar depois de
 * release_iniciar, que poe o diretorio do exe como diretorio corrente. */
void config_carregar(const char *caminho);

/* Valor da chave, ou NULL. Strings voltam sem aspas. */
const char *config_texto(const char *chave);

/* Inteiro da chave, ou `padrao` se ausente ou invalido. */
int config_int(const char *chave, int padrao);

/* true/false da chave, ou `padrao`. Aceita true/false/1/0/yes/no. */
int config_bool(const char *chave, int padrao);

/* Grava `chave = "valor"` no arquivo, trocando a linha se ela existir e
 * acrescentando se nao. As outras linhas ficam como estao — comentarios e
 * chaves que este programa nao conhece incluidos. Devolve 0 em sucesso. */
int config_gravar_texto(const char *chave, const char *valor);

#endif /* TINYTOON_CONFIG_H */
