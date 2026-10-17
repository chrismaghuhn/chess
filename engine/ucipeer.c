#define _GNU_SOURCE /* pipe2 */

#include "ucipeer.h"

#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* Reads lines until one starts with token (followed by a space or the end). */
static bool expect(UciPeer *p, const char *token, char *line, size_t size) {
    size_t n = strlen(token);
    while (fgets(line, (int)size, p->from)) {
        if (strncmp(line, token, n) == 0 && (line[n] == ' ' || line[n] == '\n' || line[n] == '\r' || !line[n]))
            return true;
    }
    return false;
}

static bool send(UciPeer *p, const char *text) {
    return fputs(text, p->to) >= 0 && fflush(p->to) == 0;
}

bool uci_peer_start(UciPeer *p, const char *path, char *const *options, int option_count, char *err,
                    size_t err_size) {
    memset(p, 0, sizeof *p);
    int to_child[2], from_child[2];
    /* Close-on-exec, so engines started by other threads don't inherit these pipes. */
    if (pipe2(to_child, O_CLOEXEC) != 0 || pipe2(from_child, O_CLOEXEC) != 0) {
        snprintf(err, err_size, "cannot create pipes");
        return false;
    }

    pid_t pid = fork();
    if (pid < 0) {
        snprintf(err, err_size, "cannot fork");
        return false;
    }
    if (pid == 0) {
        dup2(to_child[0], STDIN_FILENO);
        dup2(from_child[1], STDOUT_FILENO);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0)
            dup2(devnull, STDERR_FILENO);
        execl(path, path, (char *)NULL);
        _exit(127);
    }

    close(to_child[0]);
    close(from_child[1]);
    p->pid = pid;
    p->to = fdopen(to_child[1], "w");
    p->from = fdopen(from_child[0], "r");

    char line[4096];
    if (!p->to || !p->from || !send(p, "uci\n")) {
        snprintf(err, err_size, "cannot talk to %s", path);
        uci_peer_stop(p);
        return false;
    }
    while (fgets(line, sizeof line, p->from)) {
        if (strncmp(line, "id name ", 8) == 0) {
            line[strcspn(line, "\r\n")] = '\0';
            snprintf(p->name, sizeof p->name, "%.63s", line + 8);
        }
        if (strncmp(line, "uciok", 5) == 0)
            break;
    }
    if (feof(p->from)) {
        snprintf(err, err_size, "%s is not a UCI engine (or could not be started)", path);
        uci_peer_stop(p);
        return false;
    }

    for (int i = 0; i < option_count; i++) {
        const char *eq = strchr(options[i], '=');
        if (!eq) {
            snprintf(err, err_size, "option %s is not Name=Value", options[i]);
            uci_peer_stop(p);
            return false;
        }
        fprintf(p->to, "setoption name %.*s value %s\n", (int)(eq - options[i]), options[i], eq + 1);
    }
    if (!send(p, "isready\n") || !expect(p, "readyok", line, sizeof line)) {
        snprintf(err, err_size, "%s did not get ready", path);
        uci_peer_stop(p);
        return false;
    }
    return true;
}

bool uci_peer_new_game(UciPeer *p) {
    char line[4096];
    return send(p, "ucinewgame\nisready\n") && expect(p, "readyok", line, sizeof line);
}

bool uci_peer_go(UciPeer *p, const char *fen, const char *moves, int movetime, uint64_t nodes, char move[8]) {
    fprintf(p->to, "position fen %s%s%s\n", fen, *moves ? " moves " : "", moves);
    if (movetime > 0)
        fprintf(p->to, "go movetime %d\n", movetime);
    else
        fprintf(p->to, "go nodes %" PRIu64 "\n", nodes);
    if (fflush(p->to) != 0)
        return false;

    static _Thread_local char line[65536]; /* info lines with long PVs */
    if (!expect(p, "bestmove", line, sizeof line))
        return false;
    char *name = line + strlen("bestmove");
    name += strspn(name, " ");
    size_t len = strcspn(name, " \r\n");
    if (len < 4 || len > 5)
        return false;
    memcpy(move, name, len);
    move[len] = '\0';
    return true;
}

void uci_peer_stop(UciPeer *p) {
    if (p->to) {
        fputs("quit\n", p->to);
        fclose(p->to);
    }
    if (p->from)
        fclose(p->from);
    if (p->pid > 0)
        waitpid(p->pid, NULL, 0);
    memset(p, 0, sizeof *p);
}
