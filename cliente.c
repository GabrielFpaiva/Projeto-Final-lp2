/*
 * cliente.c
 *
 * Cliente TCP para a central de reservas.
 * Suporta modo interativo (stdin) e comando único via argumentos.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#define BUF_SIZE 512

/* lê uma linha completa do socket (até \n ou EOF) */
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
    buf[i] = '\0';
    return (ssize_t)i;
}

static void enviar(int fd, const char *msg)
{
    size_t restante = strlen(msg);
    const char *p   = msg;

    while (restante > 0) {
        ssize_t n = write(fd, p, restante);
        if (n <= 0) {
            perror("write");
            return;
        }
        p        += n;
        restante -= (size_t)n;
    }
}

/* resolve hostname e conecta via TCP */
static int conectar(const char *host, int porta)
{
    struct addrinfo hints, *res, *rp;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    char porta_str[8];
    snprintf(porta_str, sizeof(porta_str), "%d", porta);

    int err = getaddrinfo(host, porta_str, &hints, &res);
    if (err != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(err));
        return -1;
    }

    int fd = -1;
    for (rp = res; rp != NULL; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);

    if (fd < 0)
        fprintf(stderr, "Falha ao conectar em %s:%d\n", host, porta);

    return fd;
}

/* envia comando e imprime resposta; retorna 1 se recebeu BYE */
static int enviar_comando(int sockfd, const char *cmd)
{
    size_t len = strlen(cmd);
    char buf[BUF_SIZE];

    if (len > 0 && cmd[len - 1] == '\n') {
        enviar(sockfd, cmd);
    } else {
        snprintf(buf, sizeof(buf), "%s\n", cmd);
        enviar(sockfd, buf);
    }

    char resp[BUF_SIZE];
    ssize_t n = ler_linha(sockfd, resp, sizeof(resp));
    if (n < 0) {
        perror("read");
        return -1;
    }
    if (n == 0) {
        printf("Servidor fechou a conexao.\n");
        return -1;
    }

    printf("%s\n", resp);

    if (strncmp(resp, "BYE", 3) == 0) return 1;
    return 0;
}

int main(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr, "Uso: %s <host> <porta> [comando...]\n", argv[0]);
        fprintf(stderr, "\nExemplos:\n");
        fprintf(stderr, "  %s localhost 9000                  # interativo\n", argv[0]);
        fprintf(stderr, "  %s localhost 9000 RESERVE 5 Alice  # comando unico\n", argv[0]);
        return 1;
    }

    const char *host = argv[1];
    int porta = atoi(argv[2]);

    int sockfd = conectar(host, porta);
    if (sockfd < 0) return 1;

    /* comando passado por argumento: envia e encerra */
    if (argc > 3) {
        char cmd[BUF_SIZE] = {0};
        for (int i = 3; i < argc; i++) {
            if (i > 3) strncat(cmd, " ", sizeof(cmd) - strlen(cmd) - 1);
            strncat(cmd, argv[i], sizeof(cmd) - strlen(cmd) - 1);
        }
        enviar_comando(sockfd, cmd);
        close(sockfd);
        return 0;
    }

    /* modo interativo */
    printf("Conectado em %s:%d\n", host, porta);
    printf("Comandos: LIST, RESERVE <id> <nome>, CANCEL <id>, STATUS <id>, QUIT\n\n");

    char linha[BUF_SIZE];
    while (1) {
        printf("> ");
        fflush(stdout);

        if (!fgets(linha, sizeof(linha), stdin)) break;

        size_t len = strlen(linha);
        if (len > 0 && linha[len - 1] == '\n') linha[--len] = '\0';
        if (len == 0) continue;

        int rc = enviar_comando(sockfd, linha);
        if (rc != 0) break;
    }

    close(sockfd);
    return 0;
}
