// Link layer protocol implementation
// RCOM 2026/2027 - Lab 1 (M2 + M3 + M4)

#define _POSIX_C_SOURCE 200809L

#include "link_layer.h"
#include "serial_port.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// Constantes do protocolo
// ---------------------------------------------------------------------------

#define FLAG 0x7E
#define ESC 0x7D
#define STUFF_XOR 0x20

#define A_TX 0x03 // comandos do emissor / respostas do recetor
#define A_RX 0x01 // comandos do recetor / respostas do emissor (DISC/UA no fecho)

#define C_SET 0x03
#define C_UA 0x07
#define C_DISC 0x0B
#define C_I(n) ((unsigned char)((n) << 7))     // I0 = 0x00, I1 = 0x80
#define C_RR(n) ((unsigned char)(0xAA | (n)))  // RR0 = 0xAA, RR1 = 0xAB
#define C_REJ(n) ((unsigned char)(0x54 | (n))) // REJ0 = 0x54, REJ1 = 0x55

// Pior caso de uma trama I depois do stuffing:
// F A C BCC1 + (dados + BCC2) a dobrar + F
#define MAX_FRAME_SIZE (4 + 2 * (MAX_PAYLOAD_SIZE + 1) + 1)

// ---------------------------------------------------------------------------
// Tipos e estado global
// ---------------------------------------------------------------------------

typedef enum
{
    ST_START,
    ST_FLAG_RCV,
    ST_A_RCV,
    ST_C_RCV,
    ST_BCC1_OK,
    ST_DATA,
    ST_DATA_ESC,
    ST_STOP
} FrameState;

// Trama recebida, ja sem stuffing
typedef struct
{
    unsigned char a;
    unsigned char c;
    unsigned char data[MAX_PAYLOAD_SIZE + 1]; // dados + BCC2
    int hasData; // FALSE = trama de supervisao (SET, UA, RR, REJ, DISC)
    int dataLen; // bytes de dados, sem o BCC2
    int bcc2Ok;
} Frame;

static LinkLayer linkParams; // guardado no llOpen, usado no llSend
static int txNs = 0;         // Ns da proxima trama I a enviar (emissor)
static int rxExpected = 0;   // Ns da proxima trama I esperada (recetor)

static volatile sig_atomic_t alarmTriggered = FALSE;
static struct sigaction previousAct;
static int alarmInstalled = FALSE;

// ---------------------------------------------------------------------------
// Alarme
// ---------------------------------------------------------------------------

static void alarmHandler(int signalNumber)
{
    (void)signalNumber;
    alarmTriggered = TRUE;

    // Volta a armar 1 s. Se o sinal chegar entre o teste da flag e a entrada
    // no read() bloqueante, o proximo SIGALRM interrompe-o. Quem chamou
    // cancela sempre com alarm(0), por isso isto nunca dispara "a mais".
    alarm(1);
}

static int installAlarm(void)
{
    struct sigaction act;
    memset(&act, 0, sizeof(act));
    act.sa_handler = alarmHandler;
    sigemptyset(&act.sa_mask);
    act.sa_flags = 0; // sem SA_RESTART: o read() bloqueado devolve EINTR

    if (sigaction(SIGALRM, &act, &previousAct) == -1)
    {
        perror("sigaction");
        return -1;
    }
    alarmInstalled = TRUE;
    return 0;
}

static void restoreAlarm(void)
{
    if (!alarmInstalled)
        return;

    alarm(0);
    if (sigaction(SIGALRM, &previousAct, NULL) == -1)
        perror("sigaction");
    alarmInstalled = FALSE;
}

static void startTimer(void)
{
    alarmTriggered = FALSE;
    alarm(linkParams.timeout);
}

static void stopTimer(void)
{
    alarm(0);
}

// ---------------------------------------------------------------------------
// Auxiliares de envio
// ---------------------------------------------------------------------------

static const char *frameName(unsigned char c)
{
    switch (c)
    {
    case C_SET: return "SET";
    case C_UA: return "UA";
    case C_DISC: return "DISC";
    case C_RR(0): return "RR0";
    case C_RR(1): return "RR1";
    case C_REJ(0): return "REJ0";
    case C_REJ(1): return "REJ1";
    case C_I(0): return "I0";
    case C_I(1): return "I1";
    default: return "?";
    }
}

static int writeFrame(const unsigned char *frame, int len)
{
    int written = writeBytesSerialPort(frame, len);
    if (written != len)
    {
        printf("Erro a enviar trama (%d de %d bytes)\n", written, len);
        return -1;
    }
    return 0;
}

static int sendSupervisionFrame(unsigned char a, unsigned char c)
{
    unsigned char frame[5] = {FLAG, a, c, a ^ c, FLAG};

    if (writeFrame(frame, 5) < 0)
        return -1;

    printf("Enviada %s\n", frameName(c));
    return 0;
}

// Escreve um byte em out[pos] com stuffing. Devolve a nova posicao.
static int stuffByte(unsigned char *out, int pos, unsigned char byte)
{
    if (byte == FLAG || byte == ESC)
    {
        out[pos++] = ESC;
        out[pos++] = byte ^ STUFF_XOR;
    }
    else
    {
        out[pos++] = byte;
    }
    return pos;
}

// ---------------------------------------------------------------------------
// Rececao: maquina de estados unica para todas as tramas
// ---------------------------------------------------------------------------

// Le uma trama com endereco 'a'. Tramas com o cabecalho errado sao ignoradas.
// Devolve 1 se recebeu uma trama, 0 se o alarme disparou (so com useTimeout),
// -1 em caso de erro.
static int receiveFrame(unsigned char a, Frame *f, int useTimeout)
{
    FrameState state = ST_START;
    int len = 0; // bytes guardados em f->data (dados + BCC2)

    while (state != ST_STOP)
    {
        if (useTimeout && alarmTriggered)
            return 0;

        unsigned char byte;
        int res = readByteSerialPort(&byte);

        if (res < 0)
        {
            if (errno == EINTR)
                continue; // interrompido pelo alarme: o while volta a testar a flag
            perror("readByteSerialPort");
            return -1;
        }
        if (res == 0)
            continue; // nao chegou nada

        switch (state)
        {
        case ST_START:
            if (byte == FLAG)
                state = ST_FLAG_RCV;
            break;

        case ST_FLAG_RCV:
            if (byte == a)
            {
                f->a = byte;
                state = ST_A_RCV;
            }
            else if (byte != FLAG)
                state = ST_START;
            break;

        case ST_A_RCV:
            if (byte == FLAG)
                state = ST_FLAG_RCV;
            else
            {
                f->c = byte;
                state = ST_C_RCV;
            }
            break;

        case ST_C_RCV:
            if (byte == (f->a ^ f->c))
            {
                len = 0;
                state = ST_BCC1_OK;
            }
            else if (byte == FLAG)
                state = ST_FLAG_RCV;
            else
                state = ST_START; // erro no cabecalho: ignorar a trama
            break;

        case ST_BCC1_OK:
        case ST_DATA:
            if (byte == FLAG)
                state = ST_STOP; // len == 0 => trama de supervisao
            else if (byte == ESC)
                state = ST_DATA_ESC;
            else if (len < (int)sizeof(f->data))
            {
                f->data[len++] = byte;
                state = ST_DATA;
            }
            else
                state = ST_START; // trama demasiado grande: descartar
            break;

        case ST_DATA_ESC:
            if (byte == FLAG)
                state = ST_FLAG_RCV; // escape invalido: pode ser o inicio de outra trama
            else if (len < (int)sizeof(f->data))
            {
                f->data[len++] = byte ^ STUFF_XOR;
                state = ST_DATA;
            }
            else
                state = ST_START;
            break;

        default:
            state = ST_START;
            break;
        }
    }

    f->hasData = (len > 0);
    if (!f->hasData)
    {
        f->dataLen = 0;
        f->bcc2Ok = TRUE;
        return 1;
    }

    // O ultimo byte e o BCC2
    f->dataLen = len - 1;
    unsigned char bcc2 = 0;
    for (int i = 0; i < f->dataLen; i++)
        bcc2 ^= f->data[i];
    f->bcc2Ok = (bcc2 == f->data[len - 1]);

    return 1;
}

// Espera, com o alarme ligado, por uma trama de supervisao cujo C seja um
// dos aceites. As outras sao ignoradas (ex: um RR atrasado).
// Devolve 1 (com *cOut preenchido), 0 em timeout, -1 em erro.
static int waitForReply(unsigned char a, const unsigned char *accepted,
                        int nAccepted, unsigned char *cOut)
{
    Frame f;

    while (TRUE)
    {
        int r = receiveFrame(a, &f, TRUE);
        if (r <= 0)
            return r;
        if (f.hasData)
            continue;

        for (int i = 0; i < nAccepted; i++)
        {
            if (f.c == accepted[i])
            {
                *cOut = f.c;
                return 1;
            }
        }
        printf("Ignorada trama %s inesperada\n", frameName(f.c));
    }
}

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------

int llOpenTx(LinkLayer llParameters)
{
    if (llParameters.timeout <= 0 || llParameters.nRetransmissions < 0)
    {
        printf("Invalid timeout or retransmission limit\n");
        return -1;
    }

    linkParams = llParameters;
    txNs = 0;

    if (openSerialPort(linkParams.serialPort, linkParams.baudRate) < 0)
    {
        perror("openSerialPort");
        return -1;
    }
    printf("Serial port %s opened\n", linkParams.serialPort);

    // O handler fica instalado ate ao llCloseTx, porque o llSend tambem o usa
    if (installAlarm() < 0)
    {
        closeSerialPort();
        return -1;
    }

    const unsigned char accepted[] = {C_UA};

    for (int attempt = 0; attempt <= linkParams.nRetransmissions; attempt++)
    {
        if (attempt > 0)
            printf("Retransmissao SET %d de %d\n", attempt, linkParams.nRetransmissions);

        if (sendSupervisionFrame(A_TX, C_SET) < 0)
            break;

        unsigned char reply;
        startTimer();
        int r = waitForReply(A_TX, accepted, 1, &reply);
        stopTimer();

        if (r == 1)
        {
            printf("UA recebida. Ligacao estabelecida!\n");
            return 0;
        }
        if (r < 0)
            break;

        printf("Timeout: UA nao recebida.\n");
    }

    printf("Ligacao nao estabelecida.\n");
    restoreAlarm();
    closeSerialPort();
    return -1;
}

int llOpenRx(LinkLayer llParameters)
{
    linkParams = llParameters;
    rxExpected = 0;

    if (openSerialPort(linkParams.serialPort, linkParams.baudRate) < 0)
    {
        perror("openSerialPort");
        return -1;
    }
    printf("Serial port %s opened\n", linkParams.serialPort);

    Frame f;
    while (TRUE)
    {
        if (receiveFrame(A_TX, &f, FALSE) < 0)
        {
            closeSerialPort();
            return -1;
        }
        if (!f.hasData && f.c == C_SET)
            break;
    }
    printf("SET recebido.\n");

    if (sendSupervisionFrame(A_TX, C_UA) < 0)
    {
        closeSerialPort();
        return -1;
    }

    printf("Ligacao estabelecida!\n");
    return 0;
}

int llSend(const unsigned char *buf, int bufSize)
{
    if (buf == NULL || bufSize <= 0 || bufSize > MAX_PAYLOAD_SIZE)
    {
        printf("llSend: tamanho invalido (%d)\n", bufSize);
        return -1;
    }

    // Montar a trama I: F A C BCC1 [dados + BCC2 com stuffing] F
    static unsigned char frame[MAX_FRAME_SIZE];
    unsigned char c = C_I(txNs);
    int len = 0;

    frame[len++] = FLAG;
    frame[len++] = A_TX;
    frame[len++] = c;
    frame[len++] = A_TX ^ c;

    unsigned char bcc2 = 0; // calculado sobre os bytes originais
    for (int i = 0; i < bufSize; i++)
    {
        bcc2 ^= buf[i];
        len = stuffByte(frame, len, buf[i]);
    }
    len = stuffByte(frame, len, bcc2);
    frame[len++] = FLAG;

    // Respostas que interessam: RR com o numero seguinte (confirmado) ou
    // REJ com o numero desta trama (erro nos dados). O resto e ignorado.
    const unsigned char accepted[] = {C_RR(1 - txNs), C_REJ(txNs)};

    for (int attempt = 0; attempt <= linkParams.nRetransmissions; attempt++)
    {
        if (attempt > 0)
            printf("Retransmissao I%d (%d de %d)\n", txNs, attempt,
                   linkParams.nRetransmissions);

        if (writeFrame(frame, len) < 0)
            return -1;
        printf("Enviada I%d (%d bytes de dados)\n", txNs, bufSize);

        unsigned char reply;
        startTimer();
        int r = waitForReply(A_TX, accepted, 2, &reply);
        stopTimer();

        if (r < 0)
            return -1;

        if (r == 0)
        {
            printf("Timeout: sem resposta a I%d\n", txNs);
            continue;
        }

        if (reply == C_RR(1 - txNs))
        {
            printf("Recebido %s\n", frameName(reply));
            txNs = 1 - txNs;
            return bufSize;
        }

        printf("Recebido %s: erro nos dados, a retransmitir\n", frameName(reply));
    }

    printf("Limite de retransmissoes atingido (I%d)\n", txNs);
    return -1;
}

int llReceive(unsigned char *packet)
{
    if (packet == NULL)
        return -1;

    Frame f;

    while (TRUE)
    {
        if (receiveFrame(A_TX, &f, FALSE) < 0)
            return -1;

        if (!f.hasData)
        {
            // A UA do llOpen perdeu-se e o emissor voltou a mandar SET
            if (f.c == C_SET)
            {
                printf("SET repetido: a reenviar UA\n");
                if (sendSupervisionFrame(A_TX, C_UA) < 0)
                    return -1;
            }
            continue;
        }

        if (f.c != C_I(0) && f.c != C_I(1))
            continue;

        int ns = (f.c == C_I(1));

        if (!f.bcc2Ok)
        {
            if (ns == rxExpected)
            {
                printf("Erro nos dados de I%d\n", ns);
                if (sendSupervisionFrame(A_TX, C_REJ(ns)) < 0)
                    return -1;
            }
            else
            {
                // Duplicada com erro: basta confirmar o que ja temos
                if (sendSupervisionFrame(A_TX, C_RR(rxExpected)) < 0)
                    return -1;
            }
            continue;
        }

        if (ns != rxExpected)
        {
            // O RR anterior perdeu-se e o emissor reenviou: confirmar e descartar
            printf("I%d duplicada: descartada\n", ns);
            if (sendSupervisionFrame(A_TX, C_RR(rxExpected)) < 0)
                return -1;
            continue;
        }

        // Trama nova e sem erros: entregar a aplicacao e confirmar
        memcpy(packet, f.data, f.dataLen);
        printf("Recebida I%d (%d bytes de dados)\n", ns, f.dataLen);

        rxExpected = 1 - rxExpected;
        if (sendSupervisionFrame(A_TX, C_RR(rxExpected)) < 0)
            return -1;

        return f.dataLen;
    }
}

int llCloseTx()
{
    // TODO (M5): DISC -> DISC -> UA e estatisticas
    restoreAlarm();

    if (closeSerialPort() < 0)
    {
        perror("closeSerialPort");
        return -1;
    }

    return 0;
}

int llCloseRx()
{
    // TODO (M5): DISC -> DISC -> UA e estatisticas

    if (closeSerialPort() < 0)
    {
        perror("closeSerialPort");
        return -1;
    }

    return 0;
}
