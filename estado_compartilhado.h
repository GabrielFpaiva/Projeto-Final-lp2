/*
 * estado_compartilhado.h
 *
 * Header da biblioteca de estado compartilhado para o sistema de reservas.
 * O estado fica em SHM POSIX, protegido por mutex process-shared.
 * A API encapsula toda a sincronização internamente.
 */

#ifndef ESTADO_COMPARTILHADO_H
#define ESTADO_COMPARTILHADO_H

#include <pthread.h>

#define NUM_RECURSOS   64          /* recursos de 0 a 63                     */
#define MAX_TITULAR    33          /* 32 chars + '\0'                        */
#define SHM_NOME       "/central_reservas"

/* codigos de retorno */
#define RES_OK         0
#define RES_TAKEN      1
#define RES_FREE       2
#define RES_INVALID    3

typedef struct {
    int   ocupado;                 /* 0 = livre, 1 = ocupado                 */
    char  titular[MAX_TITULAR];
} recurso_t;

/* struct que fica mapeada na SHM */
typedef struct {
    pthread_mutex_t  mutex;                    /* PTHREAD_PROCESS_SHARED     */
    recurso_t        recursos[NUM_RECURSOS];
    volatile int     inicializado;             /* flag de controle           */
} reservas_t;

/* ciclo de vida */
reservas_t *reservas_criar(const char *nome);     /* cria e inicializa      */
reservas_t *reservas_anexar(const char *nome);    /* abre segmento existente */
void reservas_desanexar(reservas_t *r);           /* munmap                  */
void reservas_destruir(const char *nome, reservas_t *r); /* cleanup total   */

/* operacoes (sincronização interna) */
void reservas_list    (reservas_t *r, char *mapa);         /* mapa de 0/1   */
int  reservas_reserve (reservas_t *r, int id, const char *titular);
int  reservas_cancel  (reservas_t *r, int id);
int  reservas_status  (reservas_t *r, int id, char *titular_out);
void reservas_snapshot(reservas_t *r, recurso_t *buf);     /* copia atomica */

#endif
