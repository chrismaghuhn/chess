#ifndef UCIPEER_H
#define UCIPEER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

/*
 * Another engine, run as a child process and spoken to over UCI. Used only
 * to measure against (an absolute Elo anchor); nothing it plays is learned from.
 */
typedef struct {
    pid_t pid;
    FILE *to;   /* its stdin */
    FILE *from; /* its stdout */
    char name[64];
} UciPeer;

/* Starts the engine and sets options given as "Name=Value". */
bool uci_peer_start(UciPeer *p, const char *path, char *const *options, int option_count, char *err,
                    size_t err_size);

bool uci_peer_new_game(UciPeer *p);

/*
 * Asks for a move in the position given by fen and the moves played since
 * (UCI names, space separated). Exactly one of movetime (ms) and nodes is used.
 */
bool uci_peer_go(UciPeer *p, const char *fen, const char *moves, int movetime, uint64_t nodes, char move[8]);

void uci_peer_stop(UciPeer *p);

#endif
