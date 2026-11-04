#include "host.h"

#include "audio.h"
#include "input.h"
#include "mods.h"
#include "png.h"
#include "vdp.h"

#include <stdio.h>
#include <stdlib.h>

#ifndef TINYTOON_SDL

/* Build headless: o programa roda sem tela e despeja PNG ao sair. E' o modo
 * usado em maquina sem servidor grafico, para diagnostico.
 *
 * Com TINYTOON_WAV=arquivo, grava o audio em WAV. E' o que permite CONFERIR o
 * som sem placa de som e sem ouvido: dai da' para medir energia por canal,
 * comparar com uma gravacao de referencia, ou so' abrir num editor e ver se ha'
 * forma de onda onde deveria haver. "Parece que saiu som" nao e' verificacao. */

#include <stdio.h>
#include <string.h>

/* Pares (esquerda, direita) que um quadro rende. O anel pode ter mais de um
 * quadro acumulado, entao o vetor comporta o dobro — e o dobro DE PARES, que
 * sao dois int16 cada. Confundir as duas unidades foi um estouro de buffer. */
#define PARES_QUADRO_HL (MD_TAXA_AUDIO / 50u)

static FILE *wav;
static unsigned long wav_pares;
static int16_t wav_buf[PARES_QUADRO_HL * 2 * 2];

static void wav_cabecalho(FILE *f, unsigned long pares) {
    uint32_t dados = (uint32_t)(pares * 4);      /* estereo, 16 bits */
    uint32_t tamanho = 36 + dados;
    uint32_t taxa = MD_TAXA_AUDIO;
    uint32_t bytes_s = taxa * 4;
    fwrite("RIFF", 1, 4, f); fwrite(&tamanho, 4, 1, f); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    uint32_t fmt = 16; uint16_t pcm = 1, canais = 2, bloco = 4, bits = 16;
    fwrite(&fmt, 4, 1, f); fwrite(&pcm, 2, 1, f); fwrite(&canais, 2, 1, f);
    fwrite(&taxa, 4, 1, f); fwrite(&bytes_s, 4, 1, f);
    fwrite(&bloco, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&dados, 4, 1, f);
}

int host_init(void) {
    const char *nome = getenv("TINYTOON_WAV");
    if (!nome) return 0;
    wav = fopen(nome, "wb");
    if (!wav) { fprintf(stderr, "nao consegui gravar %s\n", nome); return 0; }
    wav_cabecalho(wav, 0);          /* tamanho corrigido no fim */
    printf("gravando audio em %s\n", nome);
    return 0;
}

void host_frame(void) {
    /* Os mods andam mesmo sem tela. Um mod que le' estado de jogo tem trabalho
     * a fazer aqui, e uma corrida headless e' justamente onde da' para rodar
     * mil quadros sem ninguem olhando. */
    mods_quadro();

    if (!wav) return;
    unsigned n = audio_drenar(wav_buf, PARES_QUADRO_HL * 2);
    if (n) fwrite(wav_buf, n * 2 * sizeof(int16_t), 1, wav);
    wav_pares += n;
}

void audio_relatorio(void) { }

/* MOD SEM TELA.
 *
 * O gancho de pixels vive no caminho de apresentacao, que so' existe com SDL.
 * Sem isto, um filtro so' seria testavel por alguem olhando um monitor — e mod
 * que nao da' para testar em automacao e' mod que ninguem consegue verificar.
 *
 * Entao a saida headless grava um SEGUNDO PNG, com os mods aplicados, ao lado
 * do tela.png limpo. Dois arquivos, e a diferenca entre eles E' o efeito do
 * mod, mensuravel byte a byte.
 *
 * O tela.png continua limpo de proposito: ele existe para depurar o
 * renderizador, e depurar renderizador com filtro por cima seria absurdo. */
static void despejar_com_mods(void) {
    if (!mods_ativos()) return;
    int w, h;
    vdp_screen_size(&w, &h);
    uint8_t *buf = malloc((size_t)w * h * 3);
    if (!buf) return;
    if (vdp_render(buf, w, h) == 0) {
        mods_pixels(buf, w, h);
        if (png_write_rgb("tela-mod.png", buf, w, h) == 0)
            printf("  gravado tela-mod.png (com %d mod(s) aplicado(s))\n",
                   mods_ativos());
    }
    free(buf);
}

void host_shutdown(void) {
    despejar_com_mods();
    if (!wav) return;
    fseek(wav, 0, SEEK_SET);
    wav_cabecalho(wav, wav_pares);
    fclose(wav);
    wav = NULL;
}

#else

#include <SDL3/SDL.h>

#include "memory.h"   /* md_frames, para localizar as falhas no tempo */
#include "mods.h"
#include "config.h"
#include <string.h>

/* A ROM e' regiao E (PAL) e o header declara isso; o jogo foi escrito para
 * 50 Hz. Rodar a 60 deixaria tudo 20% mais rapido — que e' exatamente o tipo de
 * "melhoria" que a regra de ouro manda deixar para depois. */
#define HZ 50
#define MS_POR_FRAME (1000.0 / HZ)

static SDL_Window *janela;
static SDL_Renderer *render;
static SDL_Texture *textura;

/* Definidas junto com a explicacao de cada uma, mais abaixo; declaradas aqui
 * porque host_init as chama para armar o estado inicial. */
static void aplicar_aspecto(void);
static void abrir_controle(SDL_JoystickID id);
static void alternar_rapido(void);
static void trocar_portas(void);
static void ler_config_imagem(void);
static void configurar_teclado(void);
static void configurar_pad(void);
/* Um controle por porta do console. O primeiro conectado vai para o jogador 1.
 * A ROM declara J4 (Team Player, 4 jogadores) no header, entao ha' mais o que
 * ligar depois destes dois.
 *
 * DOIS CAMINHOS, E O SEGUNDO E' O QUE FAZ CONTROLE GENERICO FUNCIONAR.
 *
 * SDL_Gamepad so' enxerga o que esta' no banco de mapeamentos do SDL. Para um
 * Xbox ou DualShock isso e' otimo: o SDL sabe onde fica cada botao e entrega
 * SOUTH/EAST/WEST prontos. Para um pad generico sem fabricante, ou para um
 * controle original de Mega Drive num adaptador USB, nao ha' mapeamento — e o
 * SDL nem gera SDL_EVENT_GAMEPAD_ADDED. O controle simplesmente nao existe para
 * o jogo, sem mensagem de erro nenhuma.
 *
 * Por isso todo dispositivo que o SDL NAO reconhece como gamepad e' aberto como
 * joystick cru e mapeado por heuristica. E' menos preciso e cobre o resto do
 * mundo. */
typedef struct {
    SDL_Gamepad *gp;        /* reconhecido: o SDL diz onde fica cada botao */
    SDL_Joystick *js;       /* cru: eixos, hats e botoes numerados */
    SDL_JoystickID id;
    int botoes, eixos, hats;
} Controle;

static Controle ctrl[MD_PORTAS];
static int tex_w, tex_h;
static uint8_t *frame;
static Uint64 proximo_ms;
static int escala = 3;

/* Avanco rapido. Declarado aqui, e nao junto de alternar_rapido, porque
 * audio_quadro o consulta e vem antes. Ver alternar_rapido para o porque de
 * cada uma das tres coisas que ele muda. */
static int rapido;

/* No avanco rapido a apresentacao e' limitada pelo RELOGIO, nao por uma
 * contagem de quadros.
 *
 * Medido nesta maquina, apresentando 1 quadro em cada N:
 *
 *     N= 1   148 qps ( 3,0x)      N= 6   348 qps ( 7,0x)
 *     N= 3   323 qps ( 6,5x)      N=20   521 qps (10,4x)
 *
 * Ou seja, o render domina o custo. Mas um N fixo e' o compromisso errado: numa
 * maquina mais rapida ele desperdica velocidade, numa mais lenta ele deixa a
 * imagem travada. Limitar a ~30 apresentacoes por segundo de tempo REAL se
 * ajusta sozinho — o olho nao aproveita mais que isso, e todo o resto do tempo
 * vai para a emulacao. */
#define MS_ENTRE_QUADROS_RAPIDO 33
static Uint64 ultimo_present;

/* Audio.
 *
 * O runtime GERA o som junto com o relogio e deposita num anel; aqui so' se
 * drena o que ja' existe. A quantidade por quadro varia, e e' por isso que o
 * anel existe — ver audio.h.
 *
 * Se a fila do dispositivo crescer demais, a leva e' descartada em vez de
 * acumular: latencia que so' sobe vira "o som esta' atrasado meio segundo",
 * pior que uma falha curta.
 *
 * PAR e' (esquerda, direita) = dois int16. O vetor comporta dois quadros de
 * pares, e cada par ocupa duas posicoes — confundir as duas unidades ja' custou
 * um estouro de buffer. */
#define PARES_QUADRO (MD_TAXA_AUDIO / HZ)
#define FILA_MAXIMA (PARES_QUADRO * 5)

static SDL_AudioStream *audio;
static int16_t amostras[PARES_QUADRO * 2 * 2];

/* Instrumentacao da saida de audio.
 *
 * Falha de audio e' o defeito mais dificil de investigar por relato, porque o
 * relato so' pode dizer "as vezes chia". Estes contadores dizem QUAL das tres
 * coisas aconteceu, e o log guarda o numero. */
static unsigned long au_descartes;   /* levas jogadas fora por fila cheia */
static unsigned long au_faltas;      /* quadros em que o anel nao tinha tudo */
static unsigned long au_fila_min = (unsigned long)-1, au_fila_max;
static unsigned long au_falta_1a, au_falta_ult, au_faltas_tarde;

/* Folga inicial, em quadros.
 *
 * Mesmo com a taxa certa, o relogio do SDL_GetTicks e o do dispositivo de audio
 * sao osciladores diferentes e derivam um do outro alguns ppm. Sem folga, essa
 * deriva encosta no zero de vez em quando e o dispositivo toca vazio. Dois
 * quadros dao 60 ms de margem — abaixo do que se percebe como atraso num jogo
 * de 50 Hz, e folga suficiente para o escalonamento do sistema operacional.
 *
 * E' tambem o ALVO do ritmo: o quadro segura ate' o dispositivo ter consumido
 * ate' aqui, e e' isso que mantem a fila neste nivel em vez de deixa-la
 * escorrer. Ver o laco de ritmo no fim de host_frame. */
#define FOLGA_QUADROS 3

static void audio_iniciar(void) {
    SDL_AudioSpec spec;
    spec.format = SDL_AUDIO_S16;
    spec.channels = 2;
    /* MD_TAXA_SAIDA, nao MD_TAXA_AUDIO: ver a explicacao em audio.h. Declarar a
     * taxa nativa do chip e entregar 17 amostras a menos por segundo secava a
     * fila a cada ~63 s. */
    spec.freq = (int)MD_TAXA_SAIDA;
    audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                      &spec, NULL, NULL);
    if (!audio) {
        fprintf(stderr, "audio indisponivel: %s (seguindo mudo)\n", SDL_GetError());
        return;
    }
    SDL_ResumeAudioStreamDevice(audio);
    printf("audio: %u Hz, estereo, %u pares por quadro, %d quadros de folga\n",
           MD_TAXA_SAIDA, PARES_QUADRO, FOLGA_QUADROS);
}

void audio_relatorio(void) {
    if (!audio) return;
    /* "0 e 0" seria mentira quando a fila nunca foi amostrada — e' o que
     * acontece numa sessao passada inteira em avanco rapido, onde o audio nao
     * vai para o dispositivo. Dizer que nao houve medicao e' diferente de
     * dizer que a medicao deu zero. */
    if (au_fila_min == (unsigned long)-1) {
        printf("audio: fila nao amostrada (sessao toda em avanco rapido)\n");
    } else {
        printf("audio: fila entre %lu e %lu pares | %lu levas descartadas "
               "| %lu quadros incompletos | %lu estouros do anel\n",
               au_fila_min, au_fila_max, au_descartes, au_faltas, audio_estourou);
    }
    if (au_faltas) {
        printf("       incompletos do quadro %lu ao %lu; %lu deles depois do "
               "quadro 600 (boot)\n", au_falta_1a, au_falta_ult, au_faltas_tarde);
    }
}

/* Drena o que a emulacao gerou desde o quadro anterior. A quantidade varia — e'
 * o relogio que decide, nao o hospedeiro —, e e' por isso que o anel existe. */
static void audio_quadro(void) {
    if (!audio) return;

    /* A folga e' enchida no PRIMEIRO quadro, nao na abertura do dispositivo.
     *
     * Enchendo em audio_iniciar, os 60 ms eram consumidos durante o ~1,75 s de
     * boot que corre antes do primeiro V-blank ser entregue — e o audio do jogo
     * comecava com a fila em zero, exatamente a condicao fragil que a folga
     * existe para evitar. Medido: a fila reportava minimo 0 em toda sessao.
     *
     * Aqui a folga chega junto com o som, que e' quando ela serve. */

    if (rapido) {
        /* Drenar sem enviar. Drenar e' obrigatorio: sem isso o anel enche e
         * audio_estourou dispara. Enviar seria pior — a 20x a fila do
         * dispositivo transborda e o som vira picote. */
        audio_drenar(amostras, PARES_QUADRO * 2);
        return;
    }
    int na_fila = SDL_GetAudioStreamAvailable(audio) / (int)(sizeof(int16_t) * 2);
    /* O primeiro quadro nao entra na estatistica: ele amostra a fila ANTES de
     * a folga ser posta, quando ela e' trivialmente zero. Contar isso fazia o
     * relatorio dizer "minimo 0" em toda sessao — um alarme que nunca
     * significou nada e que esconderia um zero de verdade. */
    if (md_frames > 1) {
        if ((unsigned long)na_fila < au_fila_min) au_fila_min = (unsigned long)na_fila;
        if ((unsigned long)na_fila > au_fila_max) au_fila_max = (unsigned long)na_fila;
    }
    /* Perfil da fila ao longo do tempo. Um contador so' nao distingue "a fila
     * oscila e volta" de "a fila esta' escorrendo para zero", e sao problemas
     * diferentes com consertos diferentes. */
    if (getenv("TINYTOON_AUDIO_PERFIL") && md_frames % 250 == 0) {
        printf("[audio] quadro %5lu: fila %5d pares (%.0f ms)\n",
               md_frames, na_fila, na_fila * 1000.0 / MD_TAXA_SAIDA);
        fflush(stdout);
    }
    if (na_fila > (int)FILA_MAXIMA) { au_descartes++; return; }
    unsigned n = audio_drenar(amostras, PARES_QUADRO * 2);

    /* Completa a folga no PRIMEIRO quadro, e so' o que FALTA para o alvo.
     *
     * Duas armadilhas, uma de cada lado:
     *
     * Enchendo a folga na abertura do dispositivo, os 60 ms eram consumidos
     * durante o ~1,75 s de boot que corre antes do primeiro V-blank — e o audio
     * do jogo comecava com a fila em zero, exatamente a condicao fragil que a
     * folga existe para evitar. Medido: minimo 0 em toda sessao.
     *
     * Empilhando a folga INTEIRA aqui, o resultado era o oposto: o anel ja'
     * acumulou durante o boot, o primeiro dreno traz ate' dois quadros de uma
     * vez, e a soma passava de FILA_MAXIMA. Medido: pico 6390 e uma leva
     * descartada, que e' um clique.
     *
     * O deficit resolve os dois: enche ate' o alvo, nem mais nem menos. */
    static int primeiro = 1;
    if (primeiro) {
        primeiro = 0;
        int falta = (int)(PARES_QUADRO * FOLGA_QUADROS) - na_fila - (int)n;
        if (falta > 0) {
            static int16_t silencio[PARES_QUADRO * 2 * FOLGA_QUADROS];
            SDL_PutAudioStreamData(audio, silencio,
                                   (int)(falta * 2 * sizeof(int16_t)));
        }
    }
    /* So' conta quando o anel estava VAZIO. A primeira versao contava qualquer
     * quadro que rendesse menos de 1065, e isso e' a variacao normal de quanto
     * o handler de V-blank ja' gerou antes do dreno — media 33% dos quadros,
     * nenhum deles um problema. Metrica que dispara no normal nao mede nada. */
    if (n == 0) {
        au_faltas++;
        if (!au_falta_1a) au_falta_1a = md_frames;
        au_falta_ult = md_frames;
        if (md_frames > 600) au_faltas_tarde++;
    }
    if (n) SDL_PutAudioStreamData(audio, amostras, (int)(n * 2 * sizeof(int16_t)));
}

/* Area de imagem fixa, independente do modo do VDP.
 *
 * O jogo alterna entre H40 (320 px) e H32 (256 px) conforme a tela: o logo da
 * Sega e a partida sao H40, o logo da Konami e o menu sao H32. Num console isso
 * NAO muda o tamanho da imagem — a area varrida pelo tubo e' a mesma, e o que
 * muda e' a largura do pixel: em H32 cada pixel e' 320/256 = 1,25x mais largo.
 *
 * Redimensionar a janela a cada troca de modo era infiel e ficava estranho.
 * A area logica fica fixa e a textura do VDP e' esticada para preenche-la, que
 * e' o que o hardware faz. */
#define LARGURA_BASE 320
#define ALTURA_BASE  224

static int criar_textura(int w, int h) {
    if (textura) SDL_DestroyTexture(textura);
    textura = SDL_CreateTexture(render, SDL_PIXELFORMAT_RGB24,
                                SDL_TEXTUREACCESS_STREAMING, w, h);
    if (!textura) return -1;
    /* Nearest, nao linear: em H40 o pixel do Mega Drive continua quadrado, e
     * suavizar seria decisao de apresentacao, posterior a fidelidade. */
    SDL_SetTextureScaleMode(textura, SDL_SCALEMODE_NEAREST);
    free(frame);
    frame = malloc((size_t)w * h * 3);
    tex_w = w; tex_h = h;
    return frame ? 0 : -1;
}

int host_init(void) {
    /* JOYSTICK explicito ao lado de GAMEPAD: e' o subsistema que enumera os
     * dispositivos que o SDL nao reconhece, que sao justamente os que o
     * caminho de gamepad perde. */
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_JOYSTICK
                  | SDL_INIT_AUDIO)) {
        fprintf(stderr, "SDL_Init falhou: %s\n", SDL_GetError());
        return -1;
    }
    int w, h;
    vdp_screen_size(&w, &h);

    /* tinytoon.toml (ver config.h). So' apresentacao: nada aqui chega ao jogo.
     *
     * Tela cheia por padrao quando o RecompLauncher abre o jogo no Big Picture
     * — quem esta' no sofa com um controle nao tem como redimensionar janela.
     * A chave do arquivo, se existir, vence: e' a escolha explicita do jogador. */
    const char *modo_rl = getenv("RECOMPLAUNCHER_MODE");
    int cheia = config_bool("fullscreen",
                            modo_rl && strcmp(modo_rl, "bigpicture") == 0);
    escala = config_int("window_scale", escala);
    if (escala < 1) escala = 1;
    if (escala > 8) escala = 8;
    ler_config_imagem();

#ifdef _WIN32
    /* Direct3D 11 fixo, e nao o que o SDL escolher.
     *
     * Os Looks do RecompLauncher (ReShade) precisam saber a API grafica, e o
     * launcher a descobre pelos imports do executavel — que, com o SDL
     * estatico carregando o backend em tempo de execucao, nao dizem nada. Com
     * a API fixa, o recomplauncher.json pode declara-la ("graphics": "d3d11")
     * e a declaracao e' verdade. Se o D3D11 falhar (driver antigo, VM sem
     * GPU), cai no que o SDL escolher: rodar sem os Looks e' melhor que nao
     * rodar. */
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "direct3d11");
#endif
    if (!SDL_CreateWindowAndRenderer("Tiny Toon Adventures: ACME All-Stars",
                                     LARGURA_BASE * escala, ALTURA_BASE * escala,
                                     SDL_WINDOW_RESIZABLE, &janela, &render)) {
#ifdef _WIN32
        printf("renderer direct3d11 falhou (%s); tentando o padrao do SDL\n",
               SDL_GetError());
        SDL_ResetHint(SDL_HINT_RENDER_DRIVER);
        if (!SDL_CreateWindowAndRenderer("Tiny Toon Adventures: ACME All-Stars",
                                         LARGURA_BASE * escala, ALTURA_BASE * escala,
                                         SDL_WINDOW_RESIZABLE, &janela, &render))
#endif
        {
            fprintf(stderr, "janela falhou: %s\n", SDL_GetError());
            return -1;
        }
    }
    printf("renderer: %s\n", SDL_GetRendererName(render));
    if (cheia && !SDL_SetWindowFullscreen(janela, true))
        printf("tela cheia falhou: %s\n", SDL_GetError());
    /* A apresentacao logica so' muda quando o usuario troca a proporcao com F.
     * Trocar de modo do VDP (H40 <-> H32) nao mexe nela — e' isso que mantem a
     * area de imagem constante, como num console de verdade. */
    aplicar_aspecto();
    if (criar_textura(w, h) != 0) return -1;

    /* SDL_GetJoysticks, e nao SDL_GetGamepads: o primeiro lista TODOS os
     * dispositivos, o segundo so' os que tem mapeamento. */
    int n = 0;
    SDL_JoystickID *ids = SDL_GetJoysticks(&n);
    for (int i = 0; i < n; i++) abrir_controle(ids[i]);
    SDL_free(ids);
    if (n == 0) printf("nenhum controle conectado (o teclado sempre funciona)\n");
    /* O estado inicial do F4. Depois de abrir os controles, porque e' isso que
     * ele troca; um controle plugado mais tarde entra na porta livre como
     * sempre. */
    if (config_bool("swap_ports", 0)) trocar_portas();

    /* Comecar acelerado e' util para medir o ganho e para corrida automatizada,
     * onde ninguem esta' na frente da janela para apertar Tab. */
    {
        /* getenv devolve ponteiro nao-nulo para variavel VAZIA, entao
         * "TINYTOON_RAPIDO= " ligaria o avanco rapido — o oposto do que quem
         * escreveu isso quis dizer. Vazio e "0" contam como desligado. */
        const char *r = getenv("TINYTOON_RAPIDO");
        if (r && r[0] && r[0] != '0') alternar_rapido();
    }

    audio_iniciar();
    proximo_ms = SDL_GetTicks();
    printf("janela %dx%d (escala %dx), %d Hz — o VDP alterna entre 320 e 256 px "
           "de largura, e a area de imagem nao muda\n",
           LARGURA_BASE, ALTURA_BASE, escala, HZ);
    /* Os controles ja' foram anunciados por abrir_controle, com o caminho que
     * cada um tomou. Repetir aqui daria duas listas para conferir. */
    /* O mapa EFETIVO, depois do tinytoon.toml: e' o que se quer ver no log
     * quando alguem diz que um botao nao responde. */
    configurar_teclado();
    configurar_pad();
    printf("F1=pausa, F2=avanco rapido, F3=imagem (1:1 exato / 1:1 "
           "preenchendo / 4:3), F4=trocar portas, Esc=sair\n");
    printf("controle: L+R+Start por 1 s = sair\n");
    fflush(stdout);
    return 0;
}

/* Teclado, um mapa por jogador — configuravel pelo tinytoon.toml.
 *
 * O PADRAO E' UMA DIVISAO FISICA, e e' o que faz dois jogadores caberem num
 * teclado so':
 *
 *   jogador 1  metade esquerda   WASD + JKL, Enter principal
 *   jogador 2  metade direita    setas + numerico (ou a fileira 1 2 3 0)
 *
 * Cada botao aceita VARIAS teclas. Serve ao numerico do jogador 2: teclado sem
 * bloco numerico existe, e a fileira de numeros de cima e' a saida obvia.
 *
 * TROCAR: chaves p1_up ... p2_start no tinytoon.toml, cada uma com um ou mais
 * nomes de tecla do SDL separados por virgula ("W", "Return", "Keypad 1",
 * "Left Shift"). Vazio desliga o botao — os presets de um jogador do
 * recomplauncher.json desligam o teclado do jogador 2 assim, para ele nao
 * disputar tecla com o 1. Nome que o SDL nao conhece vai para o log e e'
 * ignorado: um erro de digitacao nao pode deixar o jogo sem controle, entao o
 * botao volta ao padrao se NENHUM nome da lista valer.
 *
 * Isto e' apresentacao: muda qual tecla vira qual botao do controle do Mega
 * Drive, e o jogo recebe o mesmo controle de sempre. */
enum { BT_CIMA, BT_BAIXO, BT_ESQ, BT_DIR, BT_A, BT_B, BT_C, BT_START, BOTOES };
#define MAX_TECLAS 4

static const unsigned BIT_MD[BOTOES] = {
    MD_PAD_UP, MD_PAD_DOWN, MD_PAD_LEFT, MD_PAD_RIGHT,
    MD_PAD_A, MD_PAD_B, MD_PAD_C, MD_PAD_START,
};
static const char *const NOME_BOTAO[BOTOES] = {
    "up", "down", "left", "right", "a", "b", "c", "start",
};
static const char *const TECLAS_PADRAO[MD_PORTAS][BOTOES] = {
    { "W", "S", "A", "D", "J", "K", "L", "Return" },
    { "Up", "Down", "Left", "Right",
      "Keypad 1, 1", "Keypad 2, 2", "Keypad 3, 3", "Keypad 0, 0" },
};

/* SDL_SCANCODE_UNKNOWN (0) marca posicao vazia; o vetor de estado do teclado
 * nunca o liga, entao ler uma posicao vazia da' falso sem teste separado. */
static SDL_Scancode teclas[MD_PORTAS][BOTOES][MAX_TECLAS];

/* Le' uma lista "A, B, C" de nomes, chamando `um` para cada nome nao vazio.
 * Devolve quantos `um` aceitou. */
static int ler_lista(const char *lista, int (*um)(const char *nome, void *ctx),
                     void *ctx) {
    char buf[256];
    snprintf(buf, sizeof buf, "%s", lista);
    int aceitos = 0;
    for (char *p = strtok(buf, ","); p; p = strtok(NULL, ",")) {
        while (*p == ' ') p++;
        char *f = p + strlen(p);
        while (f > p && f[-1] == ' ') *--f = 0;
        if (*p) aceitos += um(p, ctx);
    }
    return aceitos;
}

typedef struct { SDL_Scancode *dest; int n; const char *chave; } CtxTecla;

static int uma_tecla(const char *nome, void *ctx) {
    CtxTecla *c = ctx;
    SDL_Scancode s = SDL_GetScancodeFromName(nome);
    if (s == SDL_SCANCODE_UNKNOWN) {
        printf("config: %s: tecla '%s' desconhecida, ignorada\n", c->chave, nome);
        return 0;
    }
    if (c->n < MAX_TECLAS) c->dest[c->n++] = s;
    return 1;
}

static void configurar_teclado(void) {
    for (int j = 0; j < MD_PORTAS; j++) {
        for (int b = 0; b < BOTOES; b++) {
            char chave[16];
            snprintf(chave, sizeof chave, "p%d_%s", j + 1, NOME_BOTAO[b]);
            const char *valor = config_texto(chave);
            CtxTecla c = { teclas[j][b], 0, chave };
            memset(teclas[j][b], 0, sizeof teclas[j][b]);
            if (valor && !*valor) continue;                 /* vazio: desligado */
            if (!valor || ler_lista(valor, uma_tecla, &c) == 0) {
                c.n = 0;
                ler_lista(TECLAS_PADRAO[j][b], uma_tecla, &c);
            }
        }
        printf("teclado jogador %d:", j + 1);
        for (int b = 0; b < BOTOES; b++) {
            printf(" %s=", NOME_BOTAO[b]);
            if (!teclas[j][b][0]) printf("-");
            for (int k = 0; k < MAX_TECLAS && teclas[j][b][k]; k++)
                printf("%s%s", k ? "/" : "", SDL_GetScancodeName(teclas[j][b][k]));
        }
        printf("\n");
    }
}

static unsigned ler_teclado(unsigned jogador) {
    const bool *k = SDL_GetKeyboardState(NULL);
    unsigned b = 0;
    for (int bt = 0; bt < BOTOES; bt++)
        for (int i = 0; i < MAX_TECLAS; i++)
            if (k[teclas[jogador][bt][i]]) { b |= BIT_MD[bt]; break; }
    return b;
}

/* O D-pad do Mega Drive e' digital. Analogico vira direcao por limiar, e o
 * limiar e' generoso de proposito: um pad barato com centro sujo dispararia
 * direcao sozinho num valor apertado. */
#define LIMIAR_EIXO 12000

/* Botoes do controle reconhecido pelo SDL — configuraveis pelo tinytoon.toml.
 *
 * PADRAO: A B C nos tres botoes de baixo, que e' a convencao dos emuladores de
 * Mega Drive — num Xbox da' X, A, B. Start no Start e tambem no Back: muito pad
 * generico reconhecido chama o botao de Start de "Back", e ficar preso na tela
 * de titulo sem conseguir comecar e' o pior lugar para isso falhar.
 *
 * TROCAR: pad_a, pad_b, pad_c, pad_start, cada um com um ou mais nomes do SDL
 * separados por virgula: south east west north (os quatro da frente; num Xbox
 * A B X Y), back start guide, leftshoulder rightshoulder, leftstick
 * rightstick, e os gatilhos lefttrigger righttrigger. Vale para os dois
 * jogadores. As direcoes (D-pad e analogico esquerdo) nao mudam.
 *
 * Controle que o SDL NAO reconhece segue a heuristica de ler_cru: la' nao se
 * sabe qual botao e' qual, entao nao ha' nome para configurar. */
enum { BOTOES_PAD = 4 };                  /* A, B, C, Start: BT_A..BT_START */
#define LIMIAR_GATILHO 16000
typedef struct { int usado, gatilho; SDL_GamepadButton botao; SDL_GamepadAxis eixo; } EntradaPad;
static EntradaPad pad[BOTOES_PAD][MAX_TECLAS];
static const char *const PAD_PADRAO[BOTOES_PAD] = { "west", "south", "east", "start, back" };

typedef struct { EntradaPad *dest; int n; const char *chave; } CtxPad;

static int um_botao(const char *nome, void *ctx) {
    CtxPad *c = ctx;
    EntradaPad e = { 1, 0, SDL_GAMEPAD_BUTTON_INVALID, SDL_GAMEPAD_AXIS_INVALID };
    /* O SDL3 nomeia os quatro botoes da frente pela posicao no controle do
     * Xbox (a b x y). Os nomes por posicao fisica sao mais claros num controle
     * de PlayStation ou Switch, entao os dois valem. */
    static const char *const POSICAO[][2] = {
        {"south", "a"}, {"east", "b"}, {"west", "x"}, {"north", "y"},
    };
    for (unsigned i = 0; i < sizeof POSICAO / sizeof *POSICAO; i++)
        if (SDL_strcasecmp(nome, POSICAO[i][0]) == 0) nome = POSICAO[i][1];
    e.botao = SDL_GetGamepadButtonFromString(nome);
    if (e.botao == SDL_GAMEPAD_BUTTON_INVALID) {
        e.eixo = SDL_GetGamepadAxisFromString(nome);
        if (e.eixo != SDL_GAMEPAD_AXIS_LEFT_TRIGGER &&
            e.eixo != SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) {
            printf("config: %s: botao '%s' desconhecido, ignorado\n", c->chave, nome);
            return 0;
        }
        e.gatilho = 1;
    }
    if (c->n < MAX_TECLAS) c->dest[c->n++] = e;
    return 1;
}

static void configurar_pad(void) {
    printf("controle:");
    for (int bt = 0; bt < BOTOES_PAD; bt++) {
        char chave[16];
        snprintf(chave, sizeof chave, "pad_%s", NOME_BOTAO[BT_A + bt]);
        const char *valor = config_texto(chave);
        CtxPad c = { pad[bt], 0, chave };
        memset(pad[bt], 0, sizeof pad[bt]);
        if (!(valor && !*valor) &&
            (!valor || ler_lista(valor, um_botao, &c) == 0)) {
            c.n = 0;
            ler_lista(PAD_PADRAO[bt], um_botao, &c);
        }
        printf(" %s=", NOME_BOTAO[BT_A + bt]);
        if (!c.n) printf("-");
        for (int i = 0; i < c.n; i++)
            printf("%s%s", i ? "/" : "", pad[bt][i].gatilho
                   ? SDL_GetGamepadStringForAxis(pad[bt][i].eixo)
                   : SDL_GetGamepadStringForButton(pad[bt][i].botao));
    }
    printf("\n");
}
static unsigned ler_reconhecido(SDL_Gamepad *g) {
    unsigned b = 0;
    struct { SDL_GamepadButton bt; unsigned md; } m[] = {
        {SDL_GAMEPAD_BUTTON_DPAD_UP,    MD_PAD_UP},
        {SDL_GAMEPAD_BUTTON_DPAD_DOWN,  MD_PAD_DOWN},
        {SDL_GAMEPAD_BUTTON_DPAD_LEFT,  MD_PAD_LEFT},
        {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, MD_PAD_RIGHT},
    };
    for (unsigned i = 0; i < sizeof m / sizeof *m; i++) {
        if (SDL_GetGamepadButton(g, m[i].bt)) b |= m[i].md;
    }
    /* A B C e Start: os do tinytoon.toml (ver configurar_pad). */
    for (int bt = 0; bt < BOTOES_PAD; bt++) {
        for (int i = 0; i < MAX_TECLAS && pad[bt][i].usado; i++) {
            const EntradaPad *e = &pad[bt][i];
            int apertado = e->gatilho
                ? SDL_GetGamepadAxis(g, e->eixo) > LIMIAR_GATILHO
                : SDL_GetGamepadButton(g, e->botao);
            if (apertado) { b |= BIT_MD[BT_A + bt]; break; }
        }
    }
    int ax = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX);
    int ay = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY);
    if (ax < -LIMIAR_EIXO) b |= MD_PAD_LEFT;
    if (ax >  LIMIAR_EIXO) b |= MD_PAD_RIGHT;
    if (ay < -LIMIAR_EIXO) b |= MD_PAD_UP;
    if (ay >  LIMIAR_EIXO) b |= MD_PAD_DOWN;
    return b;
}

/* Joystick cru: o SDL nao sabe o que e' cada botao, entao a heuristica decide.
 *
 * Direcoes vem do hat E dos dois primeiros eixos, somados. Praticamente todo
 * controle usa um dos dois, e ler os dois evita ter de adivinhar qual.
 *
 * A B C sao os botoes 0, 1 e 2. Essa e' a ordem em que quase todo dispositivo
 * numera os botoes de baixo, e num controle de Mega Drive por adaptador ela e'
 * literalmente A, B, C.
 *
 * START e' o problema de verdade, porque o indice varia com o dispositivo:
 *
 *     4 botoes   (MD 3 botoes por adaptador: A B C Start)         -> 3
 *     8 botoes   (MD 6 botoes: A B C X Y Z Start Mode)            -> 6 ou 7
 *     10+ botoes (pad de PC comum: ... 8=Select, 9=Start)         -> 9
 *
 * Em vez de escolher um e errar, ACEITA QUALQUER UM DOS CANDIDATOS. O jogo so'
 * precisa de um Start, e ficar preso na tela de titulo sem conseguir comecar e'
 * o pior lugar para a heuristica falhar. Os candidatos sao filtrados para nao
 * pegar os indices 0-2, que ja' sao A, B e C. */
static unsigned ler_cru(const Controle *c) {
    SDL_Joystick *j = c->js;
    unsigned b = 0;

    if (c->hats > 0) {
        Uint8 h = SDL_GetJoystickHat(j, 0);
        if (h & SDL_HAT_UP)    b |= MD_PAD_UP;
        if (h & SDL_HAT_DOWN)  b |= MD_PAD_DOWN;
        if (h & SDL_HAT_LEFT)  b |= MD_PAD_LEFT;
        if (h & SDL_HAT_RIGHT) b |= MD_PAD_RIGHT;
    }
    if (c->eixos >= 2) {
        int ax = SDL_GetJoystickAxis(j, 0);
        int ay = SDL_GetJoystickAxis(j, 1);
        if (ax < -LIMIAR_EIXO) b |= MD_PAD_LEFT;
        if (ax >  LIMIAR_EIXO) b |= MD_PAD_RIGHT;
        if (ay < -LIMIAR_EIXO) b |= MD_PAD_UP;
        if (ay >  LIMIAR_EIXO) b |= MD_PAD_DOWN;
    }

    if (c->botoes > 0 && SDL_GetJoystickButton(j, 0)) b |= MD_PAD_A;
    if (c->botoes > 1 && SDL_GetJoystickButton(j, 1)) b |= MD_PAD_B;
    if (c->botoes > 2 && SDL_GetJoystickButton(j, 2)) b |= MD_PAD_C;

    int candidatos[3] = { 9, 7, c->botoes - 1 };
    for (int i = 0; i < 3; i++) {
        int k = candidatos[i];
        if (k > 2 && k < c->botoes && SDL_GetJoystickButton(j, k)) b |= MD_PAD_START;
    }
    return b;
}

static unsigned ler_gamepad(unsigned jogador) {
    const Controle *c = &ctrl[jogador];
    if (c->gp) return ler_reconhecido(c->gp);
    if (c->js) return ler_cru(c);
    return 0;
}

/* Abre um dispositivo na primeira porta livre.
 *
 * Conectar um controle no meio da partida entra como jogador 2 sem tomar o
 * lugar do jogador 1. O relatorio diz o que foi reconhecido e por qual caminho,
 * porque quando um controle NAO funciona essa e' a primeira pergunta. */
static void abrir_controle(SDL_JoystickID id) {
    for (int i = 0; i < MD_PORTAS; i++) {
        if (ctrl[i].gp || ctrl[i].js) continue;
        Controle *c = &ctrl[i];
        c->id = id;
        if (SDL_IsGamepad(id)) {
            c->gp = SDL_OpenGamepad(id);
            if (!c->gp) { c->id = 0; return; }
            printf("controle %d: %s (reconhecido pelo SDL)\n",
                   i + 1, SDL_GetGamepadName(c->gp));
        } else {
            c->js = SDL_OpenJoystick(id);
            if (!c->js) { c->id = 0; return; }
            c->botoes = SDL_GetNumJoystickButtons(c->js);
            c->eixos  = SDL_GetNumJoystickAxes(c->js);
            c->hats   = SDL_GetNumJoystickHats(c->js);
            printf("controle %d: %s (generico: %d botoes, %d eixos, %d hats "
                   "— mapeado por heuristica)\n",
                   i + 1, SDL_GetJoystickName(c->js),
                   c->botoes, c->eixos, c->hats);
        }
        fflush(stdout);
        return;
    }
}

/* Troca os controles de porta.
 *
 * Com dois controles ja' plugados antes de abrir o jogo, quem fica sendo o
 * jogador 1 depende da ordem em que o SDL enumera os dispositivos — que nao
 * tem relacao com a ordem em que voce os plugou. Sem isto, cair invertido so'
 * se resolvia desplugando os dois e plugando de novo na ordem certa.
 *
 * Zera os botoes das duas portas junto. O estado de botao vive em input.c e so'
 * e' recalculado no proximo quadro; se a troca acontecer com o jogo pausado, ou
 * com um botao sendo segurado, ele ficaria preso na porta nova ate' alguem
 * apertar e soltar de novo. */
static void trocar_portas(void) {
    Controle t = ctrl[0];
    ctrl[0] = ctrl[1];
    ctrl[1] = t;
    for (unsigned j = 0; j < MD_PORTAS; j++) md_pad_set(j, 0);

    printf("portas trocadas:\n");
    for (int i = 0; i < MD_PORTAS; i++) {
        const char *nome = ctrl[i].gp ? SDL_GetGamepadName(ctrl[i].gp)
                         : ctrl[i].js ? SDL_GetJoystickName(ctrl[i].js)
                         : "(vazia — so' teclado)";
        printf("  jogador %d: %s\n", i + 1, nome ? nome : "(sem nome)");
    }
    fflush(stdout);
}

static void fechar_controle(SDL_JoystickID id) {
    for (int i = 0; i < MD_PORTAS; i++) {
        if (ctrl[i].id != id) continue;
        if (ctrl[i].gp) SDL_CloseGamepad(ctrl[i].gp);
        if (ctrl[i].js) SDL_CloseJoystick(ctrl[i].js);
        printf("controle %d desconectado\n", i + 1);
        fflush(stdout);
        ctrl[i] = (Controle){0};
    }
}

/* Pausa.
 *
 * host_frame() e' chamado de dentro do handler de V-blank, la' no fundo do
 * codigo recompilado. Bloquear aqui congela o 68000, o Z80 e os dois chips de
 * som ao mesmo tempo, porque todos andam a partir dos acessos do 68000 — que
 * param junto. Nao ha' estado a salvar nem a restaurar: e' a maquina inteira
 * parada no meio de um quadro, que e' o que pausa quer dizer.
 *
 * O audio e' pausado no dispositivo, e nao so' deixando de enfileirar: a fila
 * que ja' foi entregue continuaria tocando por uns 100 ms depois da tecla. */
static int pausado;

/* MODO DE IMAGEM. F cicla entre os tres.
 *
 *   0  1:1 PIXEL EXATO (padrao)
 *      320x224 com escala so' em fator inteiro. Todo pixel do jogo ocupa o
 *      mesmo numero de pixels de tela. Sobra borda quando a janela nao e'
 *      multiplo exato, e essa borda e' o preco da uniformidade.
 *
 *   1  1:1 PREENCHENDO A JANELA
 *      mesma proporcao, escala fracionaria. A imagem fica maior e alguns pixels
 *      saem com 3 pixels de tela e outros com 4. Quem prefere tela cheia a
 *      pixel uniforme fica aqui.
 *
 *   2  4:3
 *      a proporcao da TV de tubo, apresentando as mesmas 224 linhas num quadro
 *      logico de 320x240 (que e' 4:3 exato).
 *
 * Os dois primeiros sao 1:1 — a diferenca entre eles nao e' proporcao, e' se a
 * escala pode ser fracionaria. E' uma distincao que so' aparece em monitor, e
 * so' quando a janela e' redimensionada.
 *
 * Nada disto toca a logica do jogo: e' o retangulo em que a mesma imagem e'
 * desenhada, e mais nada. */
enum { MODO_EXATO = 0, MODO_PREENCHER = 1, MODO_43 = 2, MODOS = 3 };
static int modo = MODO_EXATO;

static int altura_logica(void) { return modo == MODO_43 ? 240 : ALTURA_BASE; }

static SDL_FRect rect_destino(void) {
    SDL_FRect r = { 0.0f, 0.0f, (float)LARGURA_BASE, (float)altura_logica() };
    return r;
}

static const char *NOMES_MODO[MODOS] = {
    "1:1 pixel-exact",
    "1:1 fill window",
    "4:3 (CRT TV)",
};

/* O modo inicial vem de image_mode no tinytoon.toml. Os nomes sao os que o
 * recomplauncher.json escreve; qualquer outra coisa fica no padrao, e o log
 * diz que nao reconheceu — um valor ignorado em silencio parece bug do
 * launcher. */
static void ler_config_imagem(void) {
    const char *v = config_texto("image_mode");
    if (!v) return;
    if (!strcmp(v, "exact"))     modo = MODO_EXATO;
    else if (!strcmp(v, "fill")) modo = MODO_PREENCHER;
    else if (!strcmp(v, "4:3"))  modo = MODO_43;
    else printf("config: image_mode '%s' nao reconhecido; usando o padrao\n", v);
}

/* Um unico dono do titulo da janela.
 *
 * Pausa e modo de imagem os dois querem escrever ali. Com duas funcoes
 * escrevendo, trocar o modo apagaria o aviso de pausa e despausar apagaria o
 * nome do modo — cada uma sobrescrevendo a outra conforme a ordem das teclas. */
static void atualizar_titulo(void) {
    if (!janela) return;
    char titulo[160];
    snprintf(titulo, sizeof titulo, "Tiny Toon Adventures: ACME All-Stars  [%s]%s",
             NOMES_MODO[modo], pausado ? "  [PAUSED - F1 to resume]"
                                : rapido ? "  [FAST FORWARD - F2 to stop]" : "");
    SDL_SetWindowTitle(janela, titulo);
}

static void aplicar_aspecto(void) {
    SDL_SetRenderLogicalPresentation(render, LARGURA_BASE, altura_logica(),
                                     modo == MODO_EXATO
                                       ? SDL_LOGICAL_PRESENTATION_INTEGER_SCALE
                                       : SDL_LOGICAL_PRESENTATION_LETTERBOX);
    printf("imagem: %s\n", NOMES_MODO[modo]);
    fflush(stdout);
    atualizar_titulo();
}

static void alternar_aspecto(void) {
    modo = (modo + 1) % MODOS;
    aplicar_aspecto();
}

/* AVANCO RAPIDO.
 *
 * Serve para chegar a uma tela distante sem jogar o caminho inteiro — foi
 * pedido para testar mais rapido, e e' exatamente para isso.
 *
 * Tres coisas mudam juntas, e as tres sao necessarias:
 *
 *   RITMO       o quadro deixa de esperar o dispositivo de audio consumir. Sem
 *               isto nada acelera, porque e' essa espera que segura os 50 Hz.
 *
 *   RENDER      so' um quadro em cada SALTO_RENDER e' desenhado. Compor e
 *               apresentar 20 imagens por quadro de monitor e' trabalho jogado
 *               fora: o monitor nao mostra nem a metade, e o custo disso e' o
 *               que limitaria a velocidade.
 *
 *   AUDIO       o anel continua sendo drenado, mas nao vai para o dispositivo.
 *               Drenar e' obrigatorio, senao o anel enche e o contador de
 *               estouro dispara; mandar seria pior, porque a 20x a fila enche
 *               e o som vira picote. Silencio e' a leitura honesta de "isto
 *               nao e' tempo real".
 *
 * Alternado, e nao segurado: navegar menu ate' uma fase leva minutos, e ninguem
 * quer ficar com o dedo numa tecla esse tempo todo. */

static void alternar_rapido(void) {
    rapido = !rapido;
    if (audio) {
        /* Pausar o dispositivo evita que ele fique repetindo o ultimo pedaco de
         * fila enquanto nada novo chega. */
        if (rapido) SDL_PauseAudioStreamDevice(audio);
        else { SDL_ClearAudioStream(audio); SDL_ResumeAudioStreamDevice(audio); }
    }
    printf("avanco rapido: %s\n", rapido ? "ligado" : "desligado");
    fflush(stdout);
    atualizar_titulo();
}

static void alternar_pausa(void) {
    pausado = !pausado;
    if (audio) {
        if (pausado) SDL_PauseAudioStreamDevice(audio);
        else SDL_ResumeAudioStreamDevice(audio);
    }
    atualizar_titulo();
    /* Ao voltar, o relogio recomeca de agora: sem isso o port tentaria
     * "recuperar" todos os quadros parados de uma vez. */
    if (!pausado) proximo_ms = SDL_GetTicks();
}

static void eventos(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_EVENT_QUIT:
            exit(0);            /* atexit despeja os PNG do estado final */
        case SDL_EVENT_KEY_DOWN:
            if (e.key.scancode == SDL_SCANCODE_ESCAPE) exit(0);
            /* TECLAS DE FUNCAO, e nao letras.
             *
             * P, Tab, F e T ficavam no meio do teclado, onde os dois jogadores
             * tem as maos. Pausar ou trocar de porta sem querer no meio de uma
             * partida e' o tipo de defeito que ninguem relata como bug — a
             * pessoa so' acha o programa estranho.
             *
             * As F fogem disso por construcao: ficam numa fileira separada, e
             * nenhum mapa de jogador chega la'. */
            if (e.key.scancode == SDL_SCANCODE_F1 && !e.key.repeat) alternar_pausa();
            if (e.key.scancode == SDL_SCANCODE_F3 && !e.key.repeat) alternar_aspecto();
            if (e.key.scancode == SDL_SCANCODE_F4 && !e.key.repeat) trocar_portas();
            if (e.key.scancode == SDL_SCANCODE_F2 && !e.key.repeat) alternar_rapido();
            break;
        /* So' o evento de JOYSTICK, e nao o de gamepad.
         *
         * Todo dispositivo gera SDL_EVENT_JOYSTICK_ADDED; so' os reconhecidos
         * geram tambem SDL_EVENT_GAMEPAD_ADDED. Tratar os dois abriria o mesmo
         * controle duas vezes e ele ocuparia as duas portas sozinho. O
         * joystick e' o evento universal, e abrir_controle decide o caminho. */
        case SDL_EVENT_JOYSTICK_ADDED:
            abrir_controle(e.jdevice.which);
            break;
        case SDL_EVENT_JOYSTICK_REMOVED:
            fechar_controle(e.jdevice.which);
            break;
        default: break;
        }
    }
}

/* SAIR PELO CONTROLE: L + R + Start segurados por um segundo.
 *
 * Pelo RecompLauncher no Big Picture o jogador pode nao ter teclado ao alcance,
 * e o Esc era a unica saida. O Mega Drive nao tem L nem R, entao a combinacao
 * nunca colide com um comando do jogo; o Start chega ao jogo durante o segundo
 * de espera (no maximo pausa a partida), e o tempo evita sair por esbarrao.
 *
 * Back conta como Start, igual ao mapeamento de jogo. So' controle
 * reconhecido: no caminho cru nao se sabe qual botao e' ombro, e chutar um
 * indice poderia fechar o jogo no meio da partida. */
#define QUADROS_PARA_SAIR HZ
static void verificar_saida_pelo_controle(void) {
    static int segurando;
    int combinacao = 0;
    for (int i = 0; i < MD_PORTAS; i++) {
        SDL_Gamepad *g = ctrl[i].gp;
        if (!g) continue;
        if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) &&
            SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) &&
            (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_START) ||
             SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_BACK)))
            combinacao = 1;
    }
    segurando = combinacao ? segurando + 1 : 0;
    if (segurando >= QUADROS_PARA_SAIR) {
        printf("saindo: L+R+Start no controle\n");
        fflush(stdout);
        exit(0);
    }
}

void host_frame(void) {
    if (!render) return;
    eventos();
    for (unsigned j = 0; j < MD_PORTAS; j++) {
        md_pad_set(j, ler_teclado(j) | ler_gamepad(j));
    }
    verificar_saida_pelo_controle();

    /* Uma vez por quadro de JOGO, e nao por quadro apresentado: um mod que le'
     * estado precisa ver todos os quadros, mesmo em avanco rapido. */
    mods_quadro();

    /* O jogo troca de modo entre telas. So' a textura acompanha; a area de
     * imagem fica onde esta'. */
    int w, h;
    vdp_screen_size(&w, &h);
    if (w != tex_w || h != tex_h) {
        if (criar_textura(w, h) != 0) return;
    }

    audio_quadro();

    /* Ver MS_ENTRE_QUADROS_RAPIDO: no avanco rapido a apresentacao e' limitada
     * pelo relogio, porque o render domina o custo e o monitor nao mostra nem a
     * metade do que seria composto. */
    SDL_FRect destino = rect_destino();
    int apresentar = !rapido || SDL_GetTicks() - ultimo_present >= MS_ENTRE_QUADROS_RAPIDO;
    if (apresentar) {
        ultimo_present = SDL_GetTicks();
        if (vdp_render(frame, w, h) == 0) {
            /* Os mods pegam a imagem PRONTA, entre compor e mandar para a
             * tela. E' o unico ponto em que um filtro ve' exatamente o que o
             * jogador veria, e o unico em que ele nao pode influenciar nada
             * alem da aparencia. */
            mods_pixels(frame, w, h);
            SDL_UpdateTexture(textura, NULL, frame, w * 3);
        }
        SDL_RenderClear(render);
        SDL_RenderTexture(render, textura, NULL, &destino);
        SDL_RenderPresent(render);
    }

    /* Segura o quadro enquanto pausado, continuando a redesenhar para a janela
     * nao ficar sem resposta. Sair pelo Esc ou pelo X continua funcionando. */
    while (pausado) {
        eventos();
        SDL_RenderClear(render);
        /* Recalculado dentro do laco: F funciona com o jogo pausado, e e' ai'
         * que da' para comparar as duas proporcoes na mesma imagem. */
        destino = rect_destino();
        SDL_RenderTexture(render, textura, NULL, &destino);
        SDL_RenderPresent(render);
        SDL_Delay(16);
    }

    /* RITMO PELO RELOGIO DO AUDIO.
     *
     * Nao por vsync — a taxa do monitor nao tem nada a ver com os 50 Hz do Mega
     * Drive. E nao mais por SDL_GetTicks, e a razao e' medida.
     *
     * Com relogio de parede, producao e consumo ficavam casados em 1065 pares
     * por quadro e SEM REALIMENTACAO NENHUMA. O nivel da fila era entao apenas
     * "o que sobrou da ultima vez": qualquer engasgo do sistema operacional
     * rebaixava a linha d'agua de vez, e nada a fazia subir de novo. Medido, a
     * fila raspava o zero repetidamente — e fila em zero e' o dispositivo
     * tocando vazio, que e' o chiado raro e sem padrao aparente.
     *
     * Segurar o quadro ate' o dispositivo ter consumido ate' o alvo inverte
     * isso: a fila fica PRESA no alvo em vez de escorrer. E o relogio passa a
     * ser o do proprio dispositivo de audio, que e' o master certo aqui — 40 ms
     * de audio faltando se ouve na hora, 1% de variacao no quadro nao se ve.
     *
     * Auto-regula nos dois sentidos: rodando adiantado, a fila cresce e a
     * espera aumenta; rodando atrasado, a fila encolhe e nao ha' espera
     * nenhuma, entao o port corre para recuperar. O limite de tempo existe para
     * o laco nao virar travamento se o dispositivo parar de consumir. */
    if (rapido) {
        /* Nenhuma espera: e' esta espera que segura os 50 Hz, e tira-la e' o
         * que de fato acelera. A velocidade passa a ser a da maquina. */
        proximo_ms = SDL_GetTicks();
    } else if (audio) {
        const int alvo = (int)(PARES_QUADRO * FOLGA_QUADROS);
        Uint64 limite = SDL_GetTicks() + 100;
        while (SDL_GetAudioStreamAvailable(audio) / (int)(sizeof(int16_t) * 2) > alvo
               && SDL_GetTicks() < limite) {
            SDL_Delay(1);
        }
        proximo_ms = SDL_GetTicks();
    } else {
        /* Sem dispositivo de audio nao ha' relogio melhor que o de parede. */
        proximo_ms += (Uint64)MS_POR_FRAME;
        Uint64 agora = SDL_GetTicks();
        if (agora < proximo_ms) SDL_Delay((Uint32)(proximo_ms - agora));
        else proximo_ms = agora;      /* atrasou: nao acumula divida */
    }
}

void host_shutdown(void) {
    if (audio) SDL_DestroyAudioStream(audio);
    for (int i = 0; i < MD_PORTAS; i++) {
        if (ctrl[i].gp) SDL_CloseGamepad(ctrl[i].gp);
        if (ctrl[i].js) SDL_CloseJoystick(ctrl[i].js);
    }
    if (textura) SDL_DestroyTexture(textura);
    if (render) SDL_DestroyRenderer(render);
    if (janela) SDL_DestroyWindow(janela);
    free(frame);
    SDL_Quit();
}

#endif /* TINYTOON_SDL */
