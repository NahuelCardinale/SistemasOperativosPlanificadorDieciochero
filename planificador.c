#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_LIMITE 500

enum { ESPERANDO, LISTA, CORRIENDO, TERMINADA, FALLIDA, ABORTADA };

typedef struct {
    char *id; char *nombre; int dur_ms; char *deps_txt;
    int *deps; int n_deps, cap_deps;
    int *sig; int n_sig, cap_sig;
    int pendientes; int estado;
} Act;

static Act *acts = NULL;
static int n_acts = 0;
static int *cola = NULL;
static int cola_ini = 0, cola_fin = 0;
static int limite = 1;
static int corriendo = 0;
static pid_t *run_pid = NULL;
static int *run_act = NULL;
static int *run_fd = NULL;
static volatile sig_atomic_t interrumpido = 0;

static void agregar(int **arr, int *n, int *cap, int v) {
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 4;
        *arr = realloc(*arr, (size_t)*cap * sizeof(int));
        if (!*arr) { perror("realloc"); exit(1); }
    }
    (*arr)[(*n)++] = v;
}

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) e--;
    *e = '\0';
    return s;
}

static int escribir_todo(int fd, const char *buf, size_t len) {
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}