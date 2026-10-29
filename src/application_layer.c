// Application layer protocol implementation
// RCOM 2026/2027 - Lab 1 (M5)

#include "application_layer.h"
#include "link_layer.h"

#include <stdio.h>
#include <string.h>

// Campo C dos pacotes
#define C_START 1
#define C_DATA 2
#define C_END 3

// Tipos dos parametros TLV
#define T_FILE_SIZE 0
#define T_FILE_NAME 1

// Cabecalho de um pacote de dados: C L2 L1
#define DATA_HEADER_SIZE 3
#define MAX_FRAGMENT_SIZE (MAX_PAYLOAD_SIZE - DATA_HEADER_SIZE)

// ---------------------------------------------------------------------------
// Pacotes de controlo: C [T L V] [T L V]
// ---------------------------------------------------------------------------

// Constroi um pacote START/END com o tamanho e o nome do ficheiro.
// Devolve o tamanho do pacote ou -1 se nao couber.
static int buildControlPacket(unsigned char c, long fileSize, const char *fileName,
                              unsigned char *packet)
{
    int len = 0;
    packet[len++] = c;

    // Tamanho: numero minimo de bytes, big-endian
    unsigned char sizeBytes[sizeof(long)];
    int nSize = 0;
    long tmp = fileSize;
    do
    {
        sizeBytes[nSize++] = tmp & 0xFF;
        tmp >>= 8;
    } while (tmp > 0 && nSize < (int)sizeof(long));

    packet[len++] = T_FILE_SIZE;
    packet[len++] = nSize;
    for (int i = nSize - 1; i >= 0; i--)
        packet[len++] = sizeBytes[i];

    // Nome
    int nameLen = strlen(fileName);
    if (nameLen > 255 || len + 2 + nameLen > MAX_PAYLOAD_SIZE)
        return -1;

    packet[len++] = T_FILE_NAME;
    packet[len++] = nameLen;
    memcpy(packet + len, fileName, nameLen);
    len += nameLen;

    return len;
}

// Le os TLV de um pacote START/END. Devolve 0 em sucesso, -1 se mal formado.
static int parseControlPacket(const unsigned char *packet, int size,
                              long *fileSize, char *fileName, int nameMax)
{
    *fileSize = -1;
    fileName[0] = '\0';

    int pos = 1; // saltar o C
    while (pos + 2 <= size)
    {
        unsigned char t = packet[pos++];
        unsigned char l = packet[pos++];
        if (pos + l > size)
            return -1;

        if (t == T_FILE_SIZE)
        {
            long v = 0;
            for (int i = 0; i < l; i++)
                v = (v << 8) | packet[pos + i];
            *fileSize = v;
        }
        else if (t == T_FILE_NAME)
        {
            int n = l < nameMax - 1 ? l : nameMax - 1;
            memcpy(fileName, packet + pos, n);
            fileName[n] = '\0';
        }
        // outros tipos: ignorar

        pos += l;
    }

    return (*fileSize >= 0 && pos == size) ? 0 : -1;
}

// ---------------------------------------------------------------------------
// Emissor
// ---------------------------------------------------------------------------

static void sendFile(LinkLayer llParameters, const char *filename)
{
    FILE *file = fopen(filename, "rb");
    if (file == NULL)
    {
        perror(filename);
        return;
    }

    fseek(file, 0, SEEK_END);
    long fileSize = ftell(file);
    fseek(file, 0, SEEK_SET);

    // So o nome, sem o caminho
    const char *name = strrchr(filename, '/');
    name = name ? name + 1 : filename;

    unsigned char packet[MAX_PAYLOAD_SIZE];
    int ctrlLen = buildControlPacket(C_START, fileSize, name, packet);
    if (ctrlLen < 0)
    {
        printf("Nome do ficheiro demasiado grande\n");
        fclose(file);
        return;
    }

    if (llOpenTx(llParameters) < 0)
    {
        fclose(file);
        return;
    }

    printf("[APP] A enviar %s (%ld bytes)\n", name, fileSize);

    if (llSend(packet, ctrlLen) < 0)
    {
        printf("[APP] Erro a enviar pacote START\n");
        llCloseTx();
        fclose(file);
        return;
    }

    long sent = 0;
    while (sent < fileSize)
    {
        int n = fread(packet + DATA_HEADER_SIZE, 1, MAX_FRAGMENT_SIZE, file);
        if (n <= 0)
        {
            printf("[APP] Erro a ler o ficheiro\n");
            llCloseTx();
            fclose(file);
            return;
        }

        packet[0] = C_DATA;
        packet[1] = (n >> 8) & 0xFF; // L2
        packet[2] = n & 0xFF;        // L1

        if (llSend(packet, DATA_HEADER_SIZE + n) < 0)
        {
            printf("[APP] Erro a enviar pacote de dados\n");
            llCloseTx();
            fclose(file);
            return;
        }

        sent += n;
        printf("[APP] Enviados %ld / %ld bytes\n", sent, fileSize);
    }

    // END repete a informacao do START
    ctrlLen = buildControlPacket(C_END, fileSize, name, packet);
    if (llSend(packet, ctrlLen) < 0)
        printf("[APP] Erro a enviar pacote END\n");
    else
        printf("[APP] Ficheiro enviado\n");

    fclose(file);
    llCloseTx();
}

// ---------------------------------------------------------------------------
// Recetor
// ---------------------------------------------------------------------------

static void receiveFile(LinkLayer llParameters, const char *filename)
{
    if (llOpenRx(llParameters) < 0)
        return;

    unsigned char packet[MAX_PAYLOAD_SIZE];
    FILE *file = NULL;
    long fileSize = -1;
    long received = 0;
    char name[256];
    int done = FALSE;

    while (!done)
    {
        int size = llReceive(packet);
        if (size < 0)
        {
            printf("[APP] Erro na rececao\n");
            break;
        }
        if (size == 0)
            continue;

        switch (packet[0])
        {
        case C_START:
            if (parseControlPacket(packet, size, &fileSize, name, sizeof(name)) < 0)
            {
                printf("[APP] Pacote START invalido\n");
                break;
            }
            printf("[APP] START: %s (%ld bytes)\n", name, fileSize);

            if (file == NULL)
            {
                // O ficheiro e gravado com o nome dado na linha de comandos
                file = fopen(filename, "wb");
                if (file == NULL)
                {
                    perror(filename);
                    done = TRUE;
                }
            }
            break;

        case C_DATA:
        {
            if (file == NULL)
            {
                printf("[APP] Pacote de dados antes do START: ignorado\n");
                break;
            }
            if (size < DATA_HEADER_SIZE)
            {
                printf("[APP] Pacote de dados demasiado pequeno\n");
                break;
            }

            int k = 256 * packet[1] + packet[2];
            if (k != size - DATA_HEADER_SIZE)
            {
                printf("[APP] Tamanho do pacote nao bate certo (%d != %d)\n",
                       k, size - DATA_HEADER_SIZE);
                break;
            }

            fwrite(packet + DATA_HEADER_SIZE, 1, k, file);
            received += k;
            printf("[APP] Recebidos %ld / %ld bytes\n", received, fileSize);
            break;
        }

        case C_END:
        {
            long endSize;
            char endName[256];
            if (parseControlPacket(packet, size, &endSize, endName, sizeof(endName)) < 0)
                printf("[APP] Pacote END invalido\n");
            else if (endSize != fileSize || strcmp(endName, name) != 0)
                printf("[APP] Aviso: END nao coincide com o START\n");

            if (received == fileSize)
                printf("[APP] Ficheiro recebido (%ld bytes)\n", received);
            else
                printf("[APP] Aviso: recebidos %ld bytes, esperados %ld\n",
                       received, fileSize);
            done = TRUE;
            break;
        }

        default:
            printf("[APP] Pacote desconhecido (C = %d)\n", packet[0]);
            break;
        }
    }

    if (file != NULL)
        fclose(file);

    llCloseRx();
}

// ---------------------------------------------------------------------------

void applicationLayer(const char *serialPort, const char *role, int baudRate,
                      int nTries, int timeout, const char *filename)
{
    LinkLayer llParameters = {
        .baudRate = baudRate,
        .nRetransmissions = nTries,
        .timeout = timeout,
    };
    strncpy(llParameters.serialPort, serialPort, sizeof(llParameters.serialPort) - 1);
    llParameters.serialPort[sizeof(llParameters.serialPort) - 1] = '\0';

    if (strcmp(role, "tx") == 0)
        sendFile(llParameters, filename);
    else if (strcmp(role, "rx") == 0)
        receiveFile(llParameters, filename);
    else
        printf("Invalid role: %s. Must be 'tx' or 'rx'.\n", role);
}