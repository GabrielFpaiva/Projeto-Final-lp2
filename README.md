# Central de Reservas — TP3 (Cenário B)

## Cenário Escolhido

**Cenário B — Central de Reservas.** O sistema gerencia 64 recursos numerados de 0 a 63. Cada recurso pode estar livre ou reservado por um titular. O estado é mantido em memória compartilhada POSIX (SHM), acessível por múltiplos processos ao mesmo tempo.

## Protocolo

Cada requisição é uma linha terminada em `\n`, com campos separados por espaço. Comandos não reconhecidos ou mal formados recebem `ERR <motivo>`.

| Comando | Resposta |
|---------|----------|
| `LIST` | `MAP <64 caracteres 0/1>` (0 = livre, 1 = ocupado) |
| `RESERVE <id> <titular>` | `OK` / `TAKEN` / `INVALID` |
| `CANCEL <id>` | `OK` / `FREE` / `INVALID` |
| `STATUS <id>` | `FREE` / `TAKEN <titular>` / `INVALID` |
| `QUIT` | `BYE` (servidor fecha a conexão) |

## Justificativa da primitiva

### Perfil de acesso

A operação central do Cenário B é o `RESERVE`, que funciona como um test-and-set: é preciso verificar se o recurso está livre e, caso esteja, marcá-lo como ocupado — tudo de forma atômica, sem janela entre a leitura e a escrita.

Se dois clientes tentam reservar o mesmo recurso ao mesmo tempo, apenas um deve conseguir. O outro precisa receber `TAKEN`.

### Por que mutex e não rwlock

A primeira alternativa considerada foi `pthread_rwlock_t`, que permitiria múltiplos leitores simultâneos em operações como `LIST` e `STATUS`. No entanto, o `RESERVE` exige exclusão mútua completa entre verificação e escrita. Com rwlock, dois leitores poderiam ver o recurso como livre ao mesmo tempo e ambos tentariam reservar, resultando em dupla reserva.

Uma possibilidade seria usar promoção de lock (reader → writer), mas o POSIX não garante que essa transição seja atômica — haveria uma janela de corrida entre soltar o read lock e adquirir o write lock. Usar write lock em todas as operações de `RESERVE` eliminaria a vantagem do rwlock.

### Primitiva escolhida: `pthread_mutex_t` com `PTHREAD_PROCESS_SHARED`

- **Corretude:** exclusão mútua total impede dupla reserva
- **Interprocessos:** o atributo `PTHREAD_PROCESS_SHARED` permite uso entre servidor e inspetor na mesma região de SHM
- **Simplicidade:** para 64 recursos, a diferença de desempenho entre mutex e rwlock é insignificante
- **Sem starvation:** mutex não sofre do problema de starvation de escritores comum em rwlocks

### Inicialização

A criação do segmento usa `shm_open` com `O_CREAT | O_EXCL`, garantindo que apenas um processo inicializa a estrutura. Um flag `inicializado` na struct permite que processos subsequentes (como o inspetor) aguardem a conclusão da inicialização antes de operar.

## Thread pool (bônus)

O servidor utiliza um pool fixo de 4 worker threads com uma fila de conexões, seguindo o padrão produtor-consumidor:

- O loop de `accept` enfileira os file descriptors das conexões aceitas
- Os workers consomem da fila e tratam cada conexão até `QUIT` ou desconexão

A fila é um buffer circular de capacidade 64, sincronizado com:

- **Mutex** para proteger head, tail e count
- **Condvar `nao_vazia`**: workers bloqueiam quando não há conexões; o accept sinaliza ao enfileirar
- **Condvar `nao_cheia`**: o accept bloqueia se a fila estiver cheia; workers sinalizam ao desenfileirar
- **Flag `encerrar`**: no shutdown, um broadcast acorda todas as threads

A sincronização do estado permanece inteiramente dentro da biblioteca-monitor. A fila do pool é local ao processo do servidor.

## Build e execução

### Compilação

```bash
make          # gera: servidor, cliente, inspetor
make clean    # remove binários
```

Requisitos: `gcc` com suporte a C17, Linux com SHM POSIX e pthreads.

### Execução

**1. Servidor** (porta via argv):

```bash
./servidor 9000
```

**2. Cliente** (host e porta via argv):

```bash
./cliente localhost 9000                  # modo interativo
./cliente localhost 9000 RESERVE 5 Alice  # comando único
./cliente localhost 9000 STATUS 5
./cliente localhost 9000 LIST
```

**3. Inspetor** (acessa a SHM diretamente, sem rede):

```bash
./inspetor
```

O inspetor pode ser executado a qualquer momento com o servidor ativo.

### Teste de concorrência

```bash
./cliente localhost 9000 RESERVE 10 Alice &
./cliente localhost 9000 RESERVE 10 Bob &
./cliente localhost 9000 RESERVE 10 Carol &
wait
# Apenas um recebe OK; os demais recebem TAKEN
```

## Estrutura

```
├── estado_compartilhado.h   # structs e protótipos da biblioteca
├── estado_compartilhado.c   # SHM + mutex pshared
├── servidor.c               # servidor TCP com thread pool
├── cliente.c                # cliente TCP
├── inspetor.c               # leitura direta da SHM
├── Makefile                 # compila os três binários
└── README.md
```
