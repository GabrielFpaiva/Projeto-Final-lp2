# ============================================================================
#  Makefile — Central de Reservas (Cenário B)
#
#  Gera os binários: servidor, cliente, inspetor
#  Compila em C17 com gcc, flags -Wall -Wextra.
# ============================================================================

CC       = gcc
CFLAGS   = -std=c17 -Wall -Wextra -pedantic -O2
LDFLAGS  = -lpthread -lrt

# Alvos principais
all: servidor cliente inspetor

# --- servidor (depende da biblioteca) ---
servidor: servidor.c estado_compartilhado.c estado_compartilhado.h
	$(CC) $(CFLAGS) -o $@ servidor.c estado_compartilhado.c $(LDFLAGS)

# --- cliente (independente da biblioteca) ---
cliente: cliente.c
	$(CC) $(CFLAGS) -o $@ cliente.c $(LDFLAGS)

# --- inspetor (depende da biblioteca) ---
inspetor: inspetor.c estado_compartilhado.c estado_compartilhado.h
	$(CC) $(CFLAGS) -o $@ inspetor.c estado_compartilhado.c $(LDFLAGS)

# --- limpeza ---
clean:
	rm -f servidor cliente inspetor

.PHONY: all clean
