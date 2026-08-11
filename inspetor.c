/*
 * inspetor.c
 *
 * Processo independente que acessa a SHM diretamente (sem rede)
 * e exibe o estado atual de todas as reservas.
 */

#define _POSIX_C_SOURCE 200809L

#include "estado_compartilhado.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    /* anexa ao segmento criado pelo servidor */
    reservas_t *r = reservas_anexar(SHM_NOME);
    if (!r) {
        fprintf(stderr, "Nao foi possivel abrir o segmento '%s'.\n", SHM_NOME);
        fprintf(stderr, "Verifique se o servidor esta em execucao.\n");
        return 1;
    }

    /* snapshot consistente (cópia sob lock) */
    recurso_t snap[NUM_RECURSOS];
    reservas_snapshot(r, snap);

    printf("╔══════════════════════════════════════════════════════╗\n");
    printf("║            INSPETOR - Central de Reservas           ║\n");
    printf("╠══════════════════════════════════════════════════════╣\n");
    printf("║  Segmento: %-40s ║\n", SHM_NOME);
    printf("║  Recursos: %-40d ║\n", NUM_RECURSOS);
    printf("╠══════╦══════════╦════════════════════════════════════╣\n");
    printf("║  ID  ║  Estado  ║  Titular                          ║\n");
    printf("╠══════╬══════════╬════════════════════════════════════╣\n");

    int ocupados = 0;
    int livres   = 0;

    for (int i = 0; i < NUM_RECURSOS; i++) {
        if (snap[i].ocupado) {
            printf("║  %2d  ║ OCUPADO  ║  %-32s  ║\n", i, snap[i].titular);
            ocupados++;
        } else {
            printf("║  %2d  ║  LIVRE   ║  %-32s  ║\n", i, "-");
            livres++;
        }
    }

    printf("╠══════╩══════════╩════════════════════════════════════╣\n");

    char resumo[64];
    int n = snprintf(resumo, sizeof(resumo), "%d ocupados, %d livres", ocupados, livres);
    printf("║  Resumo: %s", resumo);
    for (int i = 10 + n; i < 53; i++) printf(" ");
    printf("║\n");

    printf("╚══════════════════════════════════════════════════════╝\n");

    /* mapa compacto */
    printf("\nMapa (0=livre, 1=ocupado):\n");
    for (int i = 0; i < NUM_RECURSOS; i++) {
        printf("%c", snap[i].ocupado ? '1' : '0');
        if ((i + 1) % 16 == 0) printf("\n");
    }
    printf("\n");

    reservas_desanexar(r);
    return 0;
}
