#define _POSIX_C_SOURCE 200809L

#include "uci.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "game.h"
#include "movegen.h"
#include "nnue.h"
#include "perft.h"
#include "search.h"

#define ENGINE_NAME "Schach Gen0"
#define DEFAULT_HASH_MB 16
#define MAX_HASH_MB 4096

/*
 * The search runs in its own thread so that "stop" and "isready" are
 * answered while it thinks. Everything except the stop flag is only touched
 * by the main thread while no search is running.
 */
typedef struct {
    Game game;
    Weights weights;
    Network net;  /* loaded with setoption Network; hidden part may be empty */
    bool has_net;
    int contempt, knight_love; /* playing style, see search_set_style */
    Search *search;
    SearchLimits limits;
    atomic_bool stop;
    bool infinite;
    bool searching;
    pthread_t thread;
} Uci;

static void *search_thread(void *arg) {
    Uci *u = arg;
    SearchResult result = search_run(u->search, &u->game, &u->limits, true);

    /* After "go infinite" the GUI expects bestmove only once it sends "stop". */
    while (u->infinite && !atomic_load(&u->stop)) {
        struct timespec pause = { 0, 1000000 };
        nanosleep(&pause, NULL);
    }

    char move[6] = "0000";
    if (result.best)
        move_to_uci(result.best, move);
    printf("bestmove %s\n", move);
    fflush(stdout);
    return NULL;
}

static void stop_search(Uci *u) {
    if (!u->searching)
        return;
    atomic_store(&u->stop, true);
    pthread_join(u->thread, NULL);
    u->searching = false;
}

static char *skip_spaces(char *s) {
    return s + strspn(s, " \t");
}

static int next_int(char **save) {
    char *token = strtok_r(NULL, " \t", save);
    return token ? atoi(token) : 0;
}

static void cmd_position(Uci *u, char *args) {
    char *moves = strstr(args, "moves");
    if (moves) {
        *moves = '\0';
        moves += strlen("moves");
    }

    args = skip_spaces(args);
    Board board;
    bool ok;
    if (strncmp(args, "startpos", 8) == 0)
        ok = board_from_fen(&board, START_FEN);
    else if (strncmp(args, "fen", 3) == 0)
        ok = board_from_fen(&board, skip_spaces(args + 3));
    else
        ok = false;
    if (!ok) {
        printf("info string invalid position\n");
        return;
    }

    Game game;
    game_init(&game, &board);
    if (moves) {
        char *save;
        for (char *token = strtok_r(moves, " \t", &save); token; token = strtok_r(NULL, " \t", &save)) {
            Move m;
            if (!game_find_move(&game.board, token, &m)) {
                printf("info string illegal move %s, ignoring the rest\n", token);
                break;
            }
            game_play(&game, m);
        }
    }
    u->game = game;
}

static void cmd_go(Uci *u, char *args) {
    search_limits_init(&u->limits);
    u->infinite = false;
    bool limited = false;

    /* A limit that is given but zero or negative means "as little as possible", never "none". */
    char *save;
    for (char *token = strtok_r(args, " \t", &save); token; token = strtok_r(NULL, " \t", &save)) {
        if (strcmp(token, "infinite") == 0) {
            u->infinite = true;
        } else if (strcmp(token, "depth") == 0) {
            int depth = next_int(&save);
            u->limits.depth = depth > 0 ? depth : 1;
            limited = true;
        } else if (strcmp(token, "nodes") == 0) {
            char *value = strtok_r(NULL, " \t", &save);
            unsigned long long nodes = value ? strtoull(value, NULL, 10) : 0;
            u->limits.nodes = nodes > 0 ? nodes : 1;
            limited = true;
        } else if (strcmp(token, "movetime") == 0) {
            int movetime = next_int(&save);
            u->limits.movetime = movetime > 0 ? movetime : 1;
            limited = true;
        } else if (strcmp(token, "wtime") == 0) {
            int t = next_int(&save);
            u->limits.time[WHITE] = t > 0 ? t : 0;
            limited = true;
        } else if (strcmp(token, "btime") == 0) {
            int t = next_int(&save);
            u->limits.time[BLACK] = t > 0 ? t : 0;
            limited = true;
        } else if (strcmp(token, "winc") == 0) {
            u->limits.inc[WHITE] = next_int(&save);
        } else if (strcmp(token, "binc") == 0) {
            u->limits.inc[BLACK] = next_int(&save);
        } else if (strcmp(token, "movestogo") == 0) {
            u->limits.movestogo = next_int(&save);
        } else if (strcmp(token, "perft") == 0) {
            int depth = next_int(&save);
            if (depth > 0)
                perft_divide(&u->game.board, depth, stdout);
            return;
        }
    }
    /* A bare "go" searches until "stop". */
    if (!limited)
        u->infinite = true;

    atomic_store(&u->stop, false);
    u->limits.stop = &u->stop;
    if (pthread_create(&u->thread, NULL, search_thread, u) != 0) {
        printf("info string could not start search thread\n");
        return;
    }
    u->searching = true;
}

static void cmd_setoption(Uci *u, char *args) {
    char *name = strstr(args, "name");
    char *value = strstr(args, "value");
    if (!name || !value)
        return;
    *value = '\0';
    name = skip_spaces(name + strlen("name"));
    value = skip_spaces(value + strlen("value"));
    name[strcspn(name, " \t")] = '\0';

    /* Values may contain spaces (file paths), so only trailing whitespace goes. */
    size_t len = strlen(value);
    while (len > 0 && (value[len - 1] == ' ' || value[len - 1] == '\t'))
        value[--len] = '\0';

    if (strcasecmp(name, "Contempt") == 0 || strcasecmp(name, "KnightLove") == 0) {
        char *end;
        long v = strtol(value, &end, 10);
        bool contempt = strcasecmp(name, "Contempt") == 0;
        if (end == value || *end || v < 0 || v > (contempt ? 1000 : 500)) {
            printf("info string could not set %s to %s\n", name, value);
            return;
        }
        if (contempt)
            u->contempt = (int)v;
        else
            u->knight_love = (int)v;
        search_set_style(u->search, u->contempt, u->knight_love);
    } else if (strcasecmp(name, "Hash") == 0) {
        int mb = atoi(value);
        if (mb < 1 || mb > MAX_HASH_MB || !search_resize(u->search, (size_t)mb))
            printf("info string could not set Hash to %s\n", value);
    } else if (strcasecmp(name, "Weights") == 0) {
        char err[256];
        if (len == 0 || strcmp(value, "<empty>") == 0) {
            weights_zero(&u->weights);
        } else if (weights_load(value, &u->weights, err, sizeof err)) {
            printf("info string loaded weights %s\n", value);
        } else {
            printf("info string %s\n", err);
            return;
        }
        search_set_weights(u->search, &u->weights); /* recomputes the learned piece values */
        search_clear(u->search);                    /* stored scores came from the old weights */
    } else if (strcasecmp(name, "Network") == 0) {
        /* Evaluate with a network file (milestone 9); Weights still order captures. */
        char err[256];
        Network net;
        if (len == 0 || strcmp(value, "<empty>") == 0) {
            search_set_network(u->search, NULL);
            if (u->has_net)
                nnue_free(&u->net);
            u->has_net = false;
        } else if (nnue_load(&net, value, err, sizeof err)) {
            /* Switch only if the new network can be used; otherwise keep the old one. */
            Network old = u->net;
            bool had = u->has_net;
            u->net = net;
            if (search_set_network(u->search, &u->net)) {
                if (had)
                    nnue_free(&old);
                u->has_net = true;
                printf("info string loaded network %s\n", value);
            } else {
                nnue_free(&u->net);
                u->net = old;
                if (!had || !search_set_network(u->search, &u->net)) {
                    if (had)
                        nnue_free(&u->net);
                    u->has_net = false;
                    search_set_network(u->search, NULL);
                }
                printf("info string network %s cannot be used (weights too large or out of memory)\n", value);
                return;
            }
        } else {
            printf("info string %s\n", err);
            return;
        }
        search_clear(u->search);
    } else {
        printf("info string unknown option %s\n", name);
    }
}

void uci_loop(void) {
    Uci *u = calloc(1, sizeof *u);
    if (!u || !(u->search = search_new(DEFAULT_HASH_MB))) {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
    search_set_weights(u->search, &u->weights); /* zero until setoption Weights */
    Board start;
    board_from_fen(&start, START_FEN);
    game_init(&u->game, &start);

    char *line = NULL;
    size_t capacity = 0;
    bool quit = false;
    while (!quit && getline(&line, &capacity, stdin) != -1) {
        line[strcspn(line, "\r\n")] = '\0';
        char *command = skip_spaces(line);
        char *args = command + strcspn(command, " \t");
        if (*args)
            *args++ = '\0';

        if (strcmp(command, "uci") == 0) {
            printf("id name " ENGINE_NAME "\n");
            printf("id author chrismaghuhn\n");
            printf("option name Hash type spin default %d min 1 max %d\n", DEFAULT_HASH_MB, MAX_HASH_MB);
            printf("option name Weights type string default <empty>\n");
            printf("option name Network type string default <empty>\n");
            printf("option name Contempt type spin default 0 min 0 max 1000\n");
            printf("option name KnightLove type spin default 0 min 0 max 500\n");
            printf("uciok\n");
        } else if (strcmp(command, "isready") == 0) {
            printf("readyok\n");
        } else if (strcmp(command, "ucinewgame") == 0) {
            stop_search(u);
            search_clear(u->search);
            game_init(&u->game, &start);
        } else if (strcmp(command, "position") == 0) {
            stop_search(u);
            cmd_position(u, args);
        } else if (strcmp(command, "go") == 0) {
            stop_search(u);
            cmd_go(u, args);
        } else if (strcmp(command, "stop") == 0) {
            stop_search(u);
        } else if (strcmp(command, "setoption") == 0) {
            stop_search(u);
            cmd_setoption(u, args);
        } else if (strcmp(command, "d") == 0) {
            board_print(&u->game.board, stdout);
        } else if (strcmp(command, "quit") == 0) {
            quit = true;
        } else if (*command) {
            printf("info string unknown command %s\n", command);
        }
        fflush(stdout);
    }

    /* At end of input (e.g. `echo "go depth 8" | engine`) let a finite search finish. */
    if (!quit && u->searching && !u->infinite) {
        pthread_join(u->thread, NULL);
        u->searching = false;
    }
    stop_search(u);
    search_free(u->search);
    if (u->has_net)
        nnue_free(&u->net);
    free(u);
    free(line);
}
