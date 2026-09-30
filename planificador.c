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

static int leer_plan(const char *ruta) {
    FILE *f = fopen(ruta, "r");
    if (!f) { perror(ruta); return -1; }
    char *linea = NULL; size_t cap = 0; int nlinea = 0, cap_acts = 0;

    while (getline(&linea, &cap, f) != -1) {
        nlinea++;
        char *p = trim(linea);
        if (*p == '\0' || *p == '#') continue;
        char vacio[1] = {0}; char *campos[4] = {vacio, vacio, vacio, vacio};
        int nc = 0; char *ini = p;
        while (nc < 4) {
            char *c = (nc < 3) ? strchr(ini, ':') : NULL;
            campos[nc++] = ini;
            if (!c) break;
            *c = '\0'; ini = c + 1;
        }
        char *id = trim(campos[0]), *nombre = trim(campos[1]);
        char *durtxt = trim(campos[2]), *deps = trim(campos[3]);
        if (*id == '\0' || *nombre == '\0') {
            fprintf(stderr, "Linea %d: falta ID o nombre\n", nlinea);
            fclose(f); free(linea); return -1;
        }
        int dur = (*durtxt == '\0') ? (100 + rand() % 4901) : (int)strtol(durtxt, NULL, 10);
        if (n_acts == cap_acts) {
            cap_acts = cap_acts ? cap_acts * 2 : 64;
            acts = realloc(acts, (size_t)cap_acts * sizeof(Act));
        }
        Act *a = &acts[n_acts++];
        memset(a, 0, sizeof(Act));
        a->id = strdup(id); a->nombre = strdup(nombre);
        a->deps_txt = strdup(deps); a->dur_ms = dur; a->estado = ESPERANDO;
    }
    free(linea); fclose(f); return (n_acts == 0) ? -1 : 0;
}

static int cmp_idx(const void *a, const void *b) {
    return strcmp(acts[*(const int *)a].id, acts[*(const int *)b].id);
}

static int buscar(const int *orden, const char *id) {
    int lo = 0, hi = n_acts - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int c = strcmp(id, acts[orden[mid]].id);
        if (c == 0) return orden[mid];
        if (c < 0) hi = mid - 1; else lo = mid + 1;
    }
    return -1;
}

static int construir_dag(void) {
    int *orden = malloc((size_t)n_acts * sizeof(int));
    for (int i = 0; i < n_acts; i++) orden[i] = i;
    qsort(orden, (size_t)n_acts, sizeof(int), cmp_idx);
    for (int i = 0; i < n_acts; i++) {
        char *guardar;
        for (char *tok = strtok_r(acts[i].deps_txt, ",", &guardar); tok; tok = strtok_r(NULL, ",", &guardar)) {
            tok = trim(tok);
            if (*tok == '\0') continue;
            int j = buscar(orden, tok);
            agregar(&acts[i].deps, &acts[i].n_deps, &acts[i].cap_deps, j);
            agregar(&acts[j].sig, &acts[j].n_sig, &acts[j].cap_sig, i);
            acts[i].pendientes++;
        }
        free(acts[i].deps_txt); acts[i].deps_txt = NULL;
    }
    free(orden); return 0;
}

static void encolar(int i) {
    acts[i].estado = LISTA; cola[cola_fin++] = i;
}

static void abortar_rama(int origen) {
    int *pila = malloc((size_t)n_acts * sizeof(int));
    int tope = 0; pila[tope++] = origen;
    while (tope > 0) {
        int u = pila[--tope];
        for (int k = 0; k < acts[u].n_sig; k++) {
            int v = acts[u].sig[k];
            if (acts[v].estado == ESPERANDO) {
                acts[v].estado = ABORTADA;
                printf("[ABORTADA] %s (%s) depende de una actividad fallida\n", acts[v].id, acts[v].nombre);
                pila[tope++] = v;
            }
        }
    }
    free(pila);
}

static void codigo_hijo(int i, int fd_in, int fd_out) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa)); sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask); sigaction(SIGINT, &sa, NULL);

    Act *a = &acts[i]; char buf[512]; int mensajes = 0;
    for (;;) {
        ssize_t n = read(fd_in, buf, sizeof(buf));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        for (ssize_t k = 0; k < n; k++) if (buf[k] == '\n') mensajes++;
    }
    close(fd_in);
    printf("  [%s] %s recibio %d mensaje(s) de sus dependencias\n", a->id, a->nombre, mensajes);

    struct timespec ts = {a->dur_ms / 1000, (long)(a->dur_ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR);

    char msg[256];
    int len = snprintf(msg, sizeof(msg), "%.200s:OK\n", a->nombre);
    escribir_todo(fd_out, msg, (size_t)len);
    close(fd_out); _exit(0);
}

static void fallar_lanzamiento(int i) {
    acts[i].estado = FALLIDA;
    printf("[FALLIDA] %s (%s) no se pudo lanzar\n", acts[i].id, acts[i].nombre);
    abortar_rama(i);
}

static void lanzar(int i) {
    Act *a = &acts[i]; int ent[2], sal[2];
    if (pipe(ent) < 0 || pipe(sal) < 0) { fallar_lanzamiento(i); return; }
    pid_t pid = fork();
    if (pid < 0) { fallar_lanzamiento(i); return; }
    
    if (pid == 0) {
        close(ent[1]); close(sal[0]);
        for (int s = 0; s < corriendo; s++) close(run_fd[s]);
        codigo_hijo(i, ent[0], sal[1]);
    }
    close(ent[0]); close(sal[1]);
    for (int k = 0; k < a->n_deps; k++) {
        char msg[256];
        int len = snprintf(msg, sizeof(msg), "%.200s:OK\n", acts[a->deps[k]].nombre);
        if (escribir_todo(ent[1], msg, (size_t)len) < 0) break;
    }
    close(ent[1]);
    run_pid[corriendo] = pid; run_act[corriendo] = i; run_fd[corriendo++] = sal[0];
    a->estado = CORRIENDO;
    printf("[INICIO] %s (%s) pid=%d dur=%dms\n", a->id, a->nombre, (int)pid, a->dur_ms);
}

static void esperar_un_hijo(void) {
    int st; pid_t pid = waitpid(-1, &st, 0);
    if (pid < 0) return;
    int s = -1;
    for (int k = 0; k < corriendo; k++) if (run_pid[k] == pid) { s = k; break; }
    if (s < 0) return;

    int i = run_act[s]; char res[256]; size_t rl = 0;
    for (;;) {
        char tmp[128]; ssize_t n = read(run_fd[s], tmp, sizeof(tmp));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        for (ssize_t k = 0; k < n && rl < sizeof(res) - 1; k++) res[rl++] = tmp[k];
    }
    res[rl] = '\0'; close(run_fd[s]);

    corriendo--; run_pid[s] = run_pid[corriendo]; run_act[s] = run_act[corriendo]; run_fd[s] = run_fd[corriendo];

    if (interrumpido) return;

    if (WIFEXITED(st) && WEXITSTATUS(st) == 0) {
        acts[i].estado = TERMINADA;
        printf("[FIN] %s (%s) OK\n", acts[i].id, acts[i].nombre);
        for (int k = 0; k < acts[i].n_sig; k++) {
            int v = acts[i].sig[k];
            if (--acts[v].pendientes == 0 && acts[v].estado == ESPERANDO) encolar(v);
        }
    } else {
        acts[i].estado = FALLIDA;
        abortar_rama(i);
    }
}

static void manejador_sigint(int sig) {
    (void)sig; interrumpido = 1;
}

static void abortar_todo(void) {
    printf("\n[SEREMI] SIGINT recibido: abortando todas las actividades\n");
    for (int s = 0; s < corriendo; s++) kill(run_pid[s], SIGTERM);
    for (int s = 0; s < corriendo; s++) {
        while (waitpid(run_pid[s], NULL, 0) < 0 && errno == EINTR);
        close(run_fd[s]); acts[run_act[s]].estado = ABORTADA;
    }
    corriendo = 0;
}

static void liberar(void) {
    for (int i = 0; i < n_acts; i++) {
        free(acts[i].id); free(acts[i].nombre);
        free(acts[i].deps_txt); free(acts[i].deps); free(acts[i].sig);
    }
    free(acts); free(cola); free(run_pid); free(run_act); free(run_fd);
}

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "Uso: %s plan.txt K\n", argv[0]); return 1; }
    limite = (int)strtol(argv[2], NULL, 10);
    if (limite > MAX_LIMITE) limite = MAX_LIMITE;

    setvbuf(stdout, NULL, _IOLBF, 0); srand((unsigned)time(NULL));

    if (leer_plan(argv[1]) < 0 || construir_dag() < 0) { liberar(); return 1; }

    cola = malloc((size_t)n_acts * sizeof(int));
    run_pid = malloc((size_t)limite * sizeof(pid_t));
    run_act = malloc((size_t)limite * sizeof(int));
    run_fd = malloc((size_t)limite * sizeof(int));

    struct sigaction sa; memset(&sa, 0, sizeof(sa));
    sa.sa_handler = manejador_sigint; sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL); signal(SIGPIPE, SIG_IGN);

    for (int i = 0; i < n_acts; i++) if (acts[i].pendientes == 0) encolar(i);

    while (!interrumpido) {
        while (corriendo < limite && cola_ini < cola_fin && !interrumpido) lanzar(cola[cola_ini++]);
        if (corriendo == 0) break;
        esperar_un_hijo();
    }

    if (interrumpido) abortar_todo();

    int cnt[6] = {0}; for (int i = 0; i < n_acts; i++) cnt[acts[i].estado]++;
    printf("\nResumen: %d terminadas, %d fallidas, %d abortadas\n", cnt[TERMINADA], cnt[FALLIDA], cnt[ABORTADA]);

    liberar(); return interrumpido ? 130 : 0;
}