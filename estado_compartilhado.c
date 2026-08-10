/*
 * estado_compartilhado.c
 *
 * Implementação da biblioteca de estado compartilhado.
 * Gerencia o segmento SHM e as operações de reserva/cancelamento.
 * Cada função adquire e libera o mutex internamente.
 */

#define _POSIX_C_SOURCE 200809L

#include "estado_compartilhado.h"

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>

/* mapeia o segmento e fecha o fd (não é necessário após mmap) */
static reservas_t *mapear_shm(int fd)
{
    reservas_t *r = mmap(NULL, sizeof(reservas_t),
                         PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (r == MAP_FAILED) {
        perror("mmap");
        return NULL;
    }
    return r;
}

reservas_t *reservas_criar(const char *nome)
{
    /* O_EXCL garante que apenas um processo faz a inicialização */
    int fd = shm_open(nome, O_CREAT | O_EXCL | O_RDWR, 0666);
    int criou = 1;

    if (fd == -1) {
        if (errno == EEXIST) {
            fd = shm_open(nome, O_RDWR, 0666);
            criou = 0;
        }
        if (fd == -1) {
            perror("shm_open (criar)");
            return NULL;
        }
    }

    if (criou) {
        if (ftruncate(fd, (off_t)sizeof(reservas_t)) == -1) {
            perror("ftruncate");
            close(fd);
            shm_unlink(nome);
            return NULL;
        }
    }

    reservas_t *r = mapear_shm(fd);
    if (!r) {
        if (criou) shm_unlink(nome);
        return NULL;
    }

    if (criou) {
        /* mutex com atributo process-shared */
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
        pthread_mutex_init(&r->mutex, &attr);
        pthread_mutexattr_destroy(&attr);

        memset(r->recursos, 0, sizeof(r->recursos));
        r->inicializado = 1;
    }

    return r;
}

reservas_t *reservas_anexar(const char *nome)
{
    int fd = shm_open(nome, O_RDWR, 0666);
    if (fd == -1) {
        perror("shm_open (anexar)");
        return NULL;
    }

    reservas_t *r = mapear_shm(fd);
    if (!r) return NULL;

    /* aguarda a inicialização pelo processo criador (até ~1s) */
    int tentativas = 0;
    while (!r->inicializado && tentativas < 100) {
        usleep(10000);
        tentativas++;
    }
    if (!r->inicializado) {
        fprintf(stderr, "Timeout aguardando inicializacao do segmento.\n");
        munmap(r, sizeof(reservas_t));
        return NULL;
    }

    return r;
}

void reservas_desanexar(reservas_t *r)
{
    if (r) munmap(r, sizeof(reservas_t));
}

void reservas_destruir(const char *nome, reservas_t *r)
{
    if (r) {
        pthread_mutex_destroy(&r->mutex);
        munmap(r, sizeof(reservas_t));
    }
    if (nome) shm_unlink(nome);
}

/* --- operações --- */

void reservas_list(reservas_t *r, char *mapa)
{
    pthread_mutex_lock(&r->mutex);
    for (int i = 0; i < NUM_RECURSOS; i++)
        mapa[i] = r->recursos[i].ocupado ? '1' : '0';
    mapa[NUM_RECURSOS] = '\0';
    pthread_mutex_unlock(&r->mutex);
}

int reservas_reserve(reservas_t *r, int id, const char *titular)
{
    if (id < 0 || id >= NUM_RECURSOS) return RES_INVALID;

    pthread_mutex_lock(&r->mutex);

    if (r->recursos[id].ocupado) {
        pthread_mutex_unlock(&r->mutex);
        return RES_TAKEN;
    }

    /* test-and-set: verificou livre, marca como ocupado */
    r->recursos[id].ocupado = 1;
    strncpy(r->recursos[id].titular, titular, MAX_TITULAR - 1);
    r->recursos[id].titular[MAX_TITULAR - 1] = '\0';

    pthread_mutex_unlock(&r->mutex);
    return RES_OK;
}

int reservas_cancel(reservas_t *r, int id)
{
    if (id < 0 || id >= NUM_RECURSOS) return RES_INVALID;

    pthread_mutex_lock(&r->mutex);

    if (!r->recursos[id].ocupado) {
        pthread_mutex_unlock(&r->mutex);
        return RES_FREE;
    }

    r->recursos[id].ocupado = 0;
    r->recursos[id].titular[0] = '\0';

    pthread_mutex_unlock(&r->mutex);
    return RES_OK;
}

int reservas_status(reservas_t *r, int id, char *titular_out)
{
    if (id < 0 || id >= NUM_RECURSOS) return RES_INVALID;

    pthread_mutex_lock(&r->mutex);

    if (!r->recursos[id].ocupado) {
        pthread_mutex_unlock(&r->mutex);
        return RES_FREE;
    }

    if (titular_out)
        strncpy(titular_out, r->recursos[id].titular, MAX_TITULAR);

    pthread_mutex_unlock(&r->mutex);
    return RES_TAKEN;
}

void reservas_snapshot(reservas_t *r, recurso_t *buf)
{
    pthread_mutex_lock(&r->mutex);
    memcpy(buf, r->recursos, sizeof(recurso_t) * NUM_RECURSOS);
    pthread_mutex_unlock(&r->mutex);
}
