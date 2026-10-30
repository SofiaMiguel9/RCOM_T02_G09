#define _POSIX_C_SOURCE 200809L

#include "link_layer.h"
#include "serial_port.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define FLAG 0x7E
#define ESC 0x7D
#define STUFF_XOR 0x20

#define A_TX 0x03
#define A_RX 0x01

#define C_SET 0x03
#define C_UA 0x07
#define C_DISC 0x0B
#define C_I(n) ((unsigned char)((n) << 7))
#define C_RR(n) ((unsigned char)(0xAA | (n)))
#define C_REJ(n) ((unsigned char)(0x54 | (n)))

#define MAX_FRAME_SIZE (4 + 2 * (MAX_PAYLOAD_SIZE + 1) + 1)

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

typedef struct
{
    unsigned char a;
    unsigned char c;
    unsigned char data[MAX_PAYLOAD_SIZE + 1];
    int hasData;
    int dataLen;
    int bcc2Ok;
} Frame;

static LinkLayer linkParams;
static int txNs = 0;
static int rxExpected = 0;
static int rxDiscPending = FALSE;

typedef struct
{
    int framesSent;
    int framesReceived;
    int iFramesSent;
    int iFramesAccepted;
    int retransmissions;
    int timeouts;
    int rejSent;
    int rejReceived;
    int duplicates;
    long payloadBytes;
    struct timespec start;
    struct timespec transferStart;
    struct timespec transferEnd;
    int transferStarted;
} Statistics;

static Statistics stats;

static volatile sig_atomic_t alarmTriggered = FALSE;
static struct sigaction previousAct;
static int alarmInstalled = FALSE;

static void alarmHandler(int signalNumber)
{
    (void)signalNumber;
    alarmTriggered = TRUE;

    alarm(1);
}

static int installAlarm(void)
{
    struct sigaction act;
    memset(&act, 0, sizeof(act));
    act.sa_handler = alarmHandler;
    sigemptyset(&act.sa_mask);
    act.sa_flags = 0;

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
    int total = 0;
    while (total < len)
    {
        int written = writeBytesSerialPort(frame + total, len - total);
        if (written < 0)
        {
            if (errno == EINTR)
                continue;
            perror("writeBytesSerialPort");
            return -1;
        }
        if (written == 0)
        {
            printf("Erro a enviar trama (%d de %d bytes)\n", total, len);
            return -1;
        }
        total += written;
    }
    return 0;
}

static int sendSupervisionFrame(unsigned char a, unsigned char c)
{
    unsigned char frame[5] = {FLAG, a, c, a ^ c, FLAG};

    if (writeFrame(frame, 5) < 0)
        return -1;

    stats.framesSent++;
    printf("Enviada %s\n", frameName(c));
    return 0;
}

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

static int receiveFrame(unsigned char a, Frame *f, int useTimeout)
{
    FrameState state = ST_START;
    int len = 0;

    while (state != ST_STOP)
    {
        if (useTimeout && alarmTriggered)
            return 0;

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
            continue;

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
                state = ST_START;
            break;

        case ST_BCC1_OK:
        case ST_DATA:
            if (byte == FLAG)
                state = ST_STOP;
            else if (byte == ESC)
                state = ST_DATA_ESC;
            else if (len < (int)sizeof(f->data))
            {
                f->data[len++] = byte;
                state = ST_DATA;
            }
            else
                state = ST_START;
            break;

        case ST_DATA_ESC:
            if (byte == FLAG)
                state = ST_FLAG_RCV;
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

    stats.framesReceived++;
    f->hasData = (len > 0);
    if (!f->hasData)
    {
        f->dataLen = 0;
        f->bcc2Ok = TRUE;
        return 1;
    }

    f->dataLen = len - 1;
    unsigned char bcc2 = 0;
    for (int i = 0; i < f->dataLen; i++)
        bcc2 ^= f->data[i];
    f->bcc2Ok = (bcc2 == f->data[len - 1]);

    return 1;
}

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

int llOpenTx(LinkLayer llParameters)
{
    if (llParameters.timeout <= 0 || llParameters.nRetransmissions < 0)
    {
        printf("Invalid timeout or retransmission limit\n");
        return -1;
    }

    linkParams = llParameters;
    txNs = 0;
    memset(&stats, 0, sizeof(stats));
    clock_gettime(CLOCK_MONOTONIC, &stats.start);

    if (openSerialPort(linkParams.serialPort, linkParams.baudRate) < 0)
    {
        perror("openSerialPort");
        return -1;
    }
    printf("Serial port %s opened\n", linkParams.serialPort);

    if (installAlarm() < 0)
    {
        closeSerialPort();
        return -1;
    }

    const unsigned char accepted[] = {C_UA};

    for (int attempt = 0; attempt <= linkParams.nRetransmissions; attempt++)
    {
        if (attempt > 0)
        {
            stats.retransmissions++;
            printf("Retransmissao SET %d de %d\n", attempt, linkParams.nRetransmissions);
        }

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

        stats.timeouts++;
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
    rxDiscPending = FALSE;
    memset(&stats, 0, sizeof(stats));
    clock_gettime(CLOCK_MONOTONIC, &stats.start);

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

    if (!stats.transferStarted)
    {
        clock_gettime(CLOCK_MONOTONIC, &stats.transferStart);
        stats.transferStarted = TRUE;
    }

    static unsigned char frame[MAX_FRAME_SIZE];
    unsigned char c = C_I(txNs);
    int len = 0;

    frame[len++] = FLAG;
    frame[len++] = A_TX;
    frame[len++] = c;
    frame[len++] = A_TX ^ c;

    unsigned char bcc2 = 0;
    for (int i = 0; i < bufSize; i++)
    {
        bcc2 ^= buf[i];
        len = stuffByte(frame, len, buf[i]);
    }
    len = stuffByte(frame, len, bcc2);
    frame[len++] = FLAG;

    const unsigned char accepted[] = {C_RR(1 - txNs), C_REJ(txNs)};

    for (int attempt = 0; attempt <= linkParams.nRetransmissions; attempt++)
    {
        if (attempt > 0)
        {
            stats.retransmissions++;
            printf("Retransmissao I%d (%d de %d)\n", txNs, attempt,
                   linkParams.nRetransmissions);
        }

        if (writeFrame(frame, len) < 0)
            return -1;
        stats.framesSent++;
        stats.iFramesSent++;
        printf("Enviada I%d (%d bytes de dados)\n", txNs, bufSize);

        unsigned char reply;
        startTimer();
        int r = waitForReply(A_TX, accepted, 2, &reply);
        stopTimer();

        if (r < 0)
            return -1;

        if (r == 0)
        {
            stats.timeouts++;
            printf("Timeout: sem resposta a I%d\n", txNs);
            continue;
        }

        if (reply == C_RR(1 - txNs))
        {
            printf("Recebido %s\n", frameName(reply));
            stats.payloadBytes += bufSize;
            clock_gettime(CLOCK_MONOTONIC, &stats.transferEnd);
            txNs = 1 - txNs;
            return bufSize;
        }

        stats.rejReceived++;
        printf("Recebido %s: erro nos dados, a retransmitir\n", frameName(reply));
    }

    printf("Limite de retransmissoes atingido (I%d)\n", txNs);
    return -1;
}

int llReceive(unsigned char *packet)
{
    if (packet == NULL)
        return -1;

    if (!stats.transferStarted)
    {
        clock_gettime(CLOCK_MONOTONIC, &stats.transferStart);
        stats.transferStarted = TRUE;
    }

    Frame f;

    while (TRUE)
    {
        if (receiveFrame(A_TX, &f, FALSE) < 0)
            return -1;

        if (!f.hasData)
        {
            if (f.c == C_DISC)
            {
                rxDiscPending = TRUE;
                printf("DISC recebido antes do fim da transferencia\n");
                return -1;
            }
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
                stats.rejSent++;
                if (sendSupervisionFrame(A_TX, C_REJ(ns)) < 0)
                    return -1;
            }
            else
            {
                if (sendSupervisionFrame(A_TX, C_RR(rxExpected)) < 0)
                    return -1;
            }
            continue;
        }

        if (ns != rxExpected)
        {
            printf("I%d duplicada: descartada\n", ns);
            stats.duplicates++;
            if (sendSupervisionFrame(A_TX, C_RR(rxExpected)) < 0)
                return -1;
            continue;
        }

        memcpy(packet, f.data, f.dataLen);
        printf("Recebida I%d (%d bytes de dados)\n", ns, f.dataLen);
        stats.iFramesAccepted++;
        stats.payloadBytes += f.dataLen;
        clock_gettime(CLOCK_MONOTONIC, &stats.transferEnd);

        rxExpected = 1 - rxExpected;
        if (sendSupervisionFrame(A_TX, C_RR(rxExpected)) < 0)
            return -1;

        return f.dataLen;
    }
}

static void printStatistics(const char *role)
{
    struct timespec end;
    clock_gettime(CLOCK_MONOTONIC, &end);
    double elapsed = (end.tv_sec - stats.start.tv_sec) +
                     (end.tv_nsec - stats.start.tv_nsec) / 1e9;

    printf("\n===== Estatisticas (%s) =====\n", role);
    printf("Tempo total:              %.3f s\n", elapsed);
    printf("Tramas enviadas:          %d\n", stats.framesSent);
    printf("Tramas recebidas:         %d\n", stats.framesReceived);
    if (strcmp(role, "tx") == 0)
    {
        printf("Tramas I enviadas:        %d\n", stats.iFramesSent);
        printf("Retransmissoes:           %d\n", stats.retransmissions);
        printf("Timeouts:                 %d\n", stats.timeouts);
        printf("REJ recebidos:            %d\n", stats.rejReceived);
        printf("Bytes de dados enviados:  %ld\n", stats.payloadBytes);
    }
    else
    {
        printf("Tramas I aceites:         %d\n", stats.iFramesAccepted);
        printf("Tramas I duplicadas:      %d\n", stats.duplicates);
        printf("REJ enviados:             %d\n", stats.rejSent);
        printf("Retransmissoes (DISC):    %d\n", stats.retransmissions);
        printf("Timeouts:                 %d\n", stats.timeouts);
        printf("Bytes de dados recebidos: %ld\n", stats.payloadBytes);
    }
    double transferTime = (stats.transferEnd.tv_sec - stats.transferStart.tv_sec) +
                          (stats.transferEnd.tv_nsec - stats.transferStart.tv_nsec) / 1e9;
    if (stats.transferStarted && stats.payloadBytes > 0 && transferTime > 0)
    {
        double bitrate = stats.payloadBytes * 8 / transferTime;
        printf("Tempo de transferencia:   %.3f s\n", transferTime);
        printf("Debito de payload (R):    %.1f bit/s\n", bitrate);
        printf("Eficiencia (R/C):         %.4f\n", bitrate / linkParams.baudRate);
    }
    printf("================================\n");
}

int llCloseTx()
{
    int result = -1;
    const unsigned char accepted[] = {C_DISC};

    for (int attempt = 0; attempt <= linkParams.nRetransmissions; attempt++)
    {
        if (attempt > 0)
        {
            stats.retransmissions++;
            printf("Retransmissao DISC %d de %d\n", attempt, linkParams.nRetransmissions);
        }

        if (sendSupervisionFrame(A_TX, C_DISC) < 0)
            break;

        unsigned char reply;
        startTimer();
        int r = waitForReply(A_RX, accepted, 1, &reply);
        stopTimer();

        if (r < 0)
            break;
        if (r == 1)
        {
            printf("DISC recebido.\n");
            if (sendSupervisionFrame(A_RX, C_UA) == 0)
            {
                result = 0;
                alarmTriggered = FALSE;
                unsigned int closeWait = (unsigned int)linkParams.timeout *
                                         ((unsigned int)linkParams.nRetransmissions + 1);
                printf("A aguardar DISC repetidos durante %u segundos\n", closeWait);
                alarm(closeWait);
                Frame f;
                while (TRUE)
                {
                    int received = receiveFrame(A_RX, &f, TRUE);
                    if (received == 0)
                        break;
                    if (received < 0)
                    {
                        result = -1;
                        break;
                    }
                    if (!f.hasData && f.c == C_DISC)
                    {
                        printf("DISC repetido: a reenviar UA\n");
                        if (sendSupervisionFrame(A_RX, C_UA) < 0)
                        {
                            result = -1;
                            break;
                        }
                    }
                }
                stopTimer();
            }
            break;
        }

        stats.timeouts++;
        printf("Timeout: DISC nao recebido.\n");
    }

    if (result == 0)
        printf("Ligacao terminada!\n");
    else
        printf("Erro a terminar a ligacao.\n");

    printStatistics("tx");
    restoreAlarm();

    if (closeSerialPort() < 0)
    {
        perror("closeSerialPort");
        return -1;
    }

    return result;
}

int llCloseRx()
{
    int result = -1;
    Frame f;

    if (installAlarm() < 0)
    {
        closeSerialPort();
        return -1;
    }

    while (!rxDiscPending)
    {
        if (receiveFrame(A_TX, &f, FALSE) < 0)
            goto out;

        if (!f.hasData && f.c == C_DISC)
            break;

        if (f.hasData && (f.c == C_I(0) || f.c == C_I(1)))
        {
            printf("I%d duplicada: descartada\n", f.c == C_I(1));
            stats.duplicates++;
            if (sendSupervisionFrame(A_TX, C_RR(rxExpected)) < 0)
                goto out;
        }
    }
    rxDiscPending = FALSE;
    printf("DISC recebido.\n");

    const unsigned char accepted[] = {C_UA};

    for (int attempt = 0; attempt <= linkParams.nRetransmissions; attempt++)
    {
        if (attempt > 0)
        {
            stats.retransmissions++;
            printf("Retransmissao DISC %d de %d\n", attempt, linkParams.nRetransmissions);
        }

        if (sendSupervisionFrame(A_RX, C_DISC) < 0)
            break;

        unsigned char reply;
        startTimer();
        int r = waitForReply(A_RX, accepted, 1, &reply);
        stopTimer();

        if (r < 0)
            break;
        if (r == 1)
        {
            printf("UA recebida.\n");
            result = 0;
            break;
        }

        stats.timeouts++;
        printf("Timeout: UA nao recebida.\n");
    }

out:
    if (result == 0)
        printf("Ligacao terminada!\n");
    else
        printf("Erro a terminar a ligacao.\n");

    printStatistics("rx");
    restoreAlarm();

    if (closeSerialPort() < 0)
    {
        perror("closeSerialPort");
        return -1;
    }

    return result;
}
