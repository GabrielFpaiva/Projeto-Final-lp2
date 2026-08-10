/*
 * servidor.c
 *
 * Servidor TCP para a central de reservas.
 * Utiliza thread pool com fila de conexões (produtor-consumidor).
 * A sincronização do estado é feita pela biblioteca.
 */

#define _POSIX_C_SOURCE 200809L

#include "estado_compartilhado.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>

#define POOL_SIZE   4
#define QUEUE_CAP   64
#define BUF_SIZE    512

/* --- fila de conexões (buffer circular, produtor-consumidor) --- */

typedef struct {
    int             fds[QUEUE_CAP];
    int             head, tail, count;
    pthread_mutex_t mutex;
    pthread_cond_t  nao_vazia;
    pthread_cond_t  nao_cheia;
    int             encerrar;
} fila_t;

static void fila_init(fila_t *f)
{
    f->head = f->tail = f->count = 0;
    f->encerrar = 0;
    pthread_mutex_init(&f->mutex, NULL);
    pthread_cond_init(&f->nao_vazia, NULL);
    pthread_cond_init(&f->nao_cheia, NULL);
}

static void fila_destroy(fila_t *f)
{
    pthread_mutex_destroy(&f->mutex);
    pthread_cond_destroy(&f->nao_vazia);
    pthread_cond_destroy(&f->nao_cheia);
}

/* enfileira fd (bloqueia se cheia) */
static void fila_push(fila_t *f, int fd)
{
    pthread_mutex_lock(&f->mutex);

    while (f->count == QUEUE_CAP && !f->encerrar)
        pthread_cond_wait(&f->nao_cheia, &f->mutex);

    if (f->encerrar) {
        pthread_mutex_unlock(&f->mutex);
        close(fd);
        return;
    }

    f->fds[f->tail] = fd;
    f->tail = (f->tail + 1) % QUEUE_CAP;
    f->count++;

    pthread_cond_signal(&f->nao_vazia);
    pthread_mutex_unlock(&f->mutex);
}

/* desenfileira fd (bloqueia se vazia), retorna -1 no shutdown */
static int fila_pop(fila_t *f)
{
    pthread_mutex_lock(&f->mutex);

    while (f->count == 0 && !f->encerrar)
        pthread_cond_wait(&f->nao_vazia, &f->mutex);

    if (f->encerrar && f->count == 0) {
        pthread_mutex_unlock(&f->mutex);
        return -1;
    }

    int fd = f->fds[f->head];
    f->head = (f->head + 1) % QUEUE_CAP;
    f->count--;

    pthread_cond_signal(&f->nao_cheia);
    pthread_mutex_unlock(&f->mutex);
    return fd;
}

/* sinaliza encerramento para todas as threads */
static void fila_shutdown(fila_t *f)
{
    pthread_mutex_lock(&f->mutex);
    f->encerrar = 1;
    pthread_cond_broadcast(&f->nao_vazia);
    pthread_cond_broadcast(&f->nao_cheia);
    pthread_mutex_unlock(&f->mutex);
}

/* --- variáveis globais --- */

static fila_t      fila;
static reservas_t *estado       = NULL;
static int         servidor_fd  = -1;

static volatile sig_atomic_t rodando = 1;

static void sighandler(int sig) { (void)sig; rodando = 0; }

/* --- leitura/escrita no socket --- */

/* lê caractere a caractere até encontrar \n ou EOF */
static ssize_t ler_linha(int fd, char *buf, size_t max)
{
    size_t i = 0;
    char   c;

    while (i < max - 1) {
        ssize_t n = read(fd, &c, 1);
        if (n < 0)  return -1;
        if (n == 0) return (ssize_t)i;
        if (c == '\n') break;
        buf[i++] = c;
    }

    /* remove \r caso o cliente envie \r\n (telnet, Windows) */
    if (i > 0 && buf[i - 1] == '\r') i--;

    buf[i] = '\0';
    return (ssize_t)i;
}

static void enviar(int fd, const char *msg)
{
    size_t  restante = strlen(msg);
    const char *p    = msg;

    while (restante > 0) {
        ssize_t n = write(fd, p, restante);
        if (n <= 0) break;
        p        += n;
        restante -= (size_t)n;
    }
}

/* --- tratamento de protocolo para uma conexão --- */

static void tratar_cliente(int cfd)
{
    char buf[BUF_SIZE];
    char resp[BUF_SIZE];

    while (1) {
        ssize_t n = ler_linha(cfd, buf, sizeof(buf));
        if (n <= 0) break;

        char *cmd  = NULL;
        char *arg1 = NULL;
        char *arg2 = NULL;

        char *saveptr = NULL;
        cmd  = strtok_r(buf,  " \t", &saveptr);
        if (!cmd) {
            enviar(cfd, "ERR comando vazio\n");
            continue;
        }
        arg1 = strtok_r(NULL, " \t", &saveptr);
        arg2 = strtok_r(NULL, " \t", &saveptr);

        if (strcasecmp(cmd, "LIST") == 0) {
            char mapa[NUM_RECURSOS + 1];
            reservas_list(estado, mapa);
            snprintf(resp, sizeof(resp), "MAP %s\n", mapa);

        } else if (strcasecmp(cmd, "RESERVE") == 0) {
            if (!arg1 || !arg2) {
                snprintf(resp, sizeof(resp), "ERR uso: RESERVE <id> <titular>\n");
            } else {
                char *endp;
                long id = strtol(arg1, &endp, 10);
                if (*endp != '\0') {
                    snprintf(resp, sizeof(resp), "INVALID\n");
                } else {
                    int rc = reservas_reserve(estado, (int)id, arg2);
                    switch (rc) {
                        case RES_OK:      snprintf(resp, sizeof(resp), "OK\n");      break;
                        case RES_TAKEN:   snprintf(resp, sizeof(resp), "TAKEN\n");   break;
                        case RES_INVALID: snprintf(resp, sizeof(resp), "INVALID\n"); break;
                        default:          snprintf(resp, sizeof(resp), "ERR\n");      break;
                    }
                }
            }

        } else if (strcasecmp(cmd, "CANCEL") == 0) {
            if (!arg1) {
                snprintf(resp, sizeof(resp), "ERR uso: CANCEL <id>\n");
            } else {
                char *endp;
                long id = strtol(arg1, &endp, 10);
                if (*endp != '\0') {
                    snprintf(resp, sizeof(resp), "INVALID\n");
                } else {
                    int rc = reservas_cancel(estado, (int)id);
                    switch (rc) {
                        case RES_OK:      snprintf(resp, sizeof(resp), "OK\n");      break;
                        case RES_FREE:    snprintf(resp, sizeof(resp), "FREE\n");    break;
                        case RES_INVALID: snprintf(resp, sizeof(resp), "INVALID\n"); break;
                        default:          snprintf(resp, sizeof(resp), "ERR\n");      break;
                    }
                }
            }

        } else if (strcasecmp(cmd, "STATUS") == 0) {
            if (!arg1) {
                snprintf(resp, sizeof(resp), "ERR uso: STATUS <id>\n");
            } else {
                char *endp;
                long id = strtol(arg1, &endp, 10);
                if (*endp != '\0') {
                    snprintf(resp, sizeof(resp), "INVALID\n");
                } else {
                    char titular[MAX_TITULAR];
                    int rc = reservas_status(estado, (int)id, titular);
                    switch (rc) {
                        case RES_FREE:    snprintf(resp, sizeof(resp), "FREE\n");             break;
                        case RES_TAKEN:   snprintf(resp, sizeof(resp), "TAKEN %s\n", titular); break;
                        case RES_INVALID: snprintf(resp, sizeof(resp), "INVALID\n");           break;
                        default:          snprintf(resp, sizeof(resp), "ERR\n");                break;
                    }
                }
            }

        } else if (strcasecmp(cmd, "QUIT") == 0) {
            enviar(cfd, "BYE\n");
            break;

        } else {
            snprintf(resp, sizeof(resp), "ERR comando desconhecido\n");
        }

        enviar(cfd, resp);
    }

    close(cfd);
}

/* loop do worker: consome conexões da fila */
static void *worker(void *arg)
{
    (void)arg;
    while (1) {
        int cfd = fila_pop(&fila);
        if (cfd < 0) break;
        tratar_cliente(cfd);
    }
    return NULL;
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Uso: %s <porta>\n", argv[0]);
        return 1;
    }

    int porta = atoi(argv[1]);
    if (porta <= 0 || porta > 65535) {
        fprintf(stderr, "Porta invalida: %s\n", argv[1]);
        return 1;
    }

    /* tratamento de sinais */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sighandler;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    /* inicializa estado compartilhado */
    estado = reservas_criar(SHM_NOME);
    if (!estado) {
        fprintf(stderr, "Erro ao criar estado compartilhado\n");
        return 1;
    }

    /* configura socket TCP */
    servidor_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (servidor_fd < 0) {
        perror("socket");
        reservas_destruir(SHM_NOME, estado);
        return 1;
    }

    int opt = 1;
    setsockopt(servidor_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons((uint16_t)porta);

    if (bind(servidor_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(servidor_fd);
        reservas_destruir(SHM_NOME, estado);
        return 1;
    }

    if (listen(servidor_fd, 16) < 0) {
        perror("listen");
        close(servidor_fd);
        reservas_destruir(SHM_NOME, estado);
        return 1;
    }

    printf("Servidor na porta %d (%d workers)\n", porta, POOL_SIZE);

    /* inicializa pool de threads */
    fila_init(&fila);
    pthread_t workers[POOL_SIZE];
    for (int i = 0; i < POOL_SIZE; i++)
        pthread_create(&workers[i], NULL, worker, NULL);

    /* accept loop: enfileira conexões para os workers */
    while (rodando) {
        struct sockaddr_in caddr;
        socklen_t clen = sizeof(caddr);
        int cfd = accept(servidor_fd, (struct sockaddr *)&caddr, &clen);

        if (cfd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }

        printf("[+] cliente %s:%d\n",
               inet_ntoa(caddr.sin_addr), ntohs(caddr.sin_port));

        fila_push(&fila, cfd);
    }

    /* encerramento */
    printf("\nEncerrando...\n");

    fila_shutdown(&fila);
    for (int i = 0; i < POOL_SIZE; i++)
        pthread_join(workers[i], NULL);
    fila_destroy(&fila);

    close(servidor_fd);
    reservas_destruir(SHM_NOME, estado);

    return 0;
}
