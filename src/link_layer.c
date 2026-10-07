// RCOM 2026/2027
//
// Link layer protocol implementation

#define _POSIX_C_SOURCE 200809L

#include "link_layer.h"
#include "serial_port.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

// MISC
#define BUF_SIZE 256

////////////////////////////////////////////////
// CONSTANTES DO PROTOCOLO
////////////////////////////////////////////////
#define FLAG 0x7E

#define A_TX 0x03 // tramas do emissor / respostas do recetor
#define A_RX 0x01 // tramas do recetor / respostas do emissor

#define C_SET 0x03  // 0x03 identifa comandos do emissor e respostas do recetor
// como SET é comando do emissor e UA resposta recetor a esse comando, usamos o mesmo
#define C_UA  0x07

////////////////////////////////////////////////
// MÁQUINA DE ESTADOS
////////////////////////////////////////////////
typedef enum
{
    ST_START,
    ST_FLAG_RCV,
    ST_A_RCV,
    ST_C_RCV,
    ST_BCC_OK,
    ST_STOP
} FrameState;

static volatile sig_atomic_t alarmTriggered = FALSE;

static void alarmHandler(int signalNumber)
{
    (void)signalNumber;
    alarmTriggered = TRUE;
}

// Envia uma trama de supervisão [FLAG, A, C, BCC1, FLAG]
//monta o array e escreve-o na porta
static int sendSupervisionFrame(unsigned char a, unsigned char c)
{
    unsigned char frame[5] = {FLAG, a, c, a ^ c, FLAG};

    int bytes = writeBytesSerialPort(frame, 5);
    if (bytes != 5)
    {
        printf("Erro a enviar trama (enviados %d bytes)\n", bytes);
        return -1;
    }

    printf("Enviada trama: ");
    for (int i = 0; i < 5; i++)
        printf("0x%02X ", frame[i]);
    printf("\n");

    return 0;
}

// Lê byte a byte até reconhecer uma trama de supervisão válida
// com o endereço 'a' e o controlo 'c' esperados.
// Returns 0 for a valid frame, 1 for a timeout, or -1 for a read error.
static int receiveSupervisionFrame(unsigned char a, unsigned char c, int useTimeout)
{
    FrameState state = ST_START;

    while (state != ST_STOP && (!useTimeout || !alarmTriggered))
    {
        unsigned char byte;
        int res = readByteSerialPort(&byte);

        if (res < 0)
        {
            if (errno == EINTR)
                continue;
            perror("readByteSerialPort");
            return -1;
        }
        if (res == 0)
            continue; // nenhum byte lido, tentar outra vez

        printf("Byte recebido: 0x%02X\n", byte);

        switch (state)
        {
        case ST_START:
            if (byte == FLAG)
                state = ST_FLAG_RCV;
            break;

        case ST_FLAG_RCV:
            if (byte == a)
                state = ST_A_RCV;
            else if (byte != FLAG) // FLAG repetida -> fica onde está
                state = ST_START;
            break;

        case ST_A_RCV:
            if (byte == c)
                state = ST_C_RCV;
            else if (byte == FLAG)
                state = ST_FLAG_RCV;
            else
                state = ST_START;
            break;

        case ST_C_RCV:
            if (byte == (a ^ c))
                state = ST_BCC_OK;
            else if (byte == FLAG)
                state = ST_FLAG_RCV;
            else
                state = ST_START; // BCC errado -> descarta a trama
            break;

        case ST_BCC_OK:
            if (byte == FLAG)
                state = ST_STOP;
            else
                state = ST_START;
            break;

        default:
            state = ST_START;
            break;
        }
    }

    return state == ST_STOP ? 0 : 1;
}

////////////////////////////////////////////////
// LLOPEN
////////////////////////////////////////////////
int llOpenTx(LinkLayer llParameters)
{
    if (llParameters.timeout <= 0 || llParameters.nRetransmissions < 0)
    {
        printf("Invalid timeout or retransmission limit\n");
        return -1;
    }

    if (openSerialPort(llParameters.serialPort, llParameters.baudRate) < 0)
    {
        perror("openSerialPort");
        return -1;
    }

    printf("Serial port %s opened\n", llParameters.serialPort);

    struct sigaction act = {0};
    struct sigaction previousAct;
    act.sa_handler = alarmHandler;
    sigemptyset(&act.sa_mask);

    // Leave SA_RESTART disabled so SIGALRM interrupts the blocking serial read.
    if (sigaction(SIGALRM, &act, &previousAct) == -1)
    {
        perror("sigaction");
        closeSerialPort();
        return -1;
    }

    int result = -1;
    for (int attempt = 0; ; attempt++)
    {
        if (sendSupervisionFrame(A_TX, C_SET) < 0)
            break;

        alarmTriggered = FALSE;
        alarm(llParameters.timeout);
        int received = receiveSupervisionFrame(A_TX, C_UA, TRUE);
        alarm(0);

        if (received == 0)
        {
            printf("UA recebida. Ligacao estabelecida!\n");
            result = 0;
            break;
        }
        if (received < 0)
            break;

        printf("Timeout: UA nao recebida.\n");
        if (attempt == llParameters.nRetransmissions)
        {
            printf("Limite de retransmissoes atingido. Ligacao nao estabelecida.\n");
            break;
        }
        printf("Retransmissao SET %d de %d\n", attempt + 1,
               llParameters.nRetransmissions);
    }

    alarm(0);
    if (sigaction(SIGALRM, &previousAct, NULL) == -1)
    {
        perror("sigaction");
        result = -1;
    }
    if (result < 0)
        closeSerialPort();

    // NOTA: a porta fica aberta, vai ser usada no llSend e fechada no llCloseTx
    return result;
}

int llOpenRx(LinkLayer llParameters)
{
    if (openSerialPort(llParameters.serialPort, llParameters.baudRate) < 0)
    {
        perror("openSerialPort");
        return -1;
    }

    printf("Serial port %s opened\n", llParameters.serialPort);

    // 1. Esperar pelo SET (enviado pelo emissor: A = 0x03)
    if (receiveSupervisionFrame(A_TX, C_SET, FALSE) < 0)
        return -1;

    printf("SET recebido.\n");

    // 2. Responder com UA
    if (sendSupervisionFrame(A_TX, C_UA) < 0)
        return -1;

    printf("UA enviada. Ligacao estabelecida!\n");

    return 0;
}

////////////////////////////////////////////////
// LLSEND
////////////////////////////////////////////////
int llSend(const unsigned char *buf, int bufSize)
{
    // TODO: Implement this function

    return 0;
}

////////////////////////////////////////////////
// LLRECEIVE
////////////////////////////////////////////////
int llReceive(unsigned char *packet)
{
    // TODO: Implement this function

    return 0;
}

////////////////////////////////////////////////
// LLCLOSE
////////////////////////////////////////////////
int llCloseTx()
{
    // TODO: DISC -> DISC -> UA

    // Para já só fecha a porta
    if (closeSerialPort() < 0)
    {
        perror("closeSerialPort");
        return -1;
    }

    return 0;
}

int llCloseRx()
{
    // TODO: DISC -> DISC -> UA

    // Para já só fecha a porta
    if (closeSerialPort() < 0)
    {
        perror("closeSerialPort");
        return -1;
    }

    return 0;
}
