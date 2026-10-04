#define _DEFAULT_SOURCE

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>

#include "../../3_gpos/esp32_link.h"

#define ESP32_DEV_CMD "/dev/esp32link_cmd"
#define ESP32_DEV_DATA "/dev/esp32link_data"

#define MAX_PUNTOS 512
#define PROMPT "app_cli> "

static int fd_cmd, fd_data;
static pthread_mutex_t consola = PTHREAD_MUTEX_INITIALIZER;

struct punto
{
    int i;
    long freq;
    double db;
};

struct barrido
{
    long frec_inicio, frec_final, puntos, tiempo;
    int recibidos;
    struct punto pts[MAX_PUNTOS];
};

static struct barrido barrido;

/* Texto de cada ERR n, segun uart_status_e del firmware */
static const char *errores[] = {
    "",
    "comando desconocido",
    "error de sintaxis",
    "valor fuera de rango",
    "configuracion invalida para iniciar",
    "el comando no aplica al estado actual"};

/* ===================== barrido ===================== */

static void imprimir_tabla(int con_puntos)
{
    int k;

    if (barrido.puntos == 0)
    {
        printf("todavia no se recibio ningun barrido\n");
        return;
    }
    printf("barrido: frec_inicio=%ld frec_final=%ld puntos=%ld tiempo=%ld\n",
           barrido.frec_inicio, barrido.frec_final, barrido.puntos, barrido.tiempo);
    if (con_puntos)
    {
        printf("%5s  %10s  %8s\n", "i", "freq (Hz)", "dB");
        for (k = 0; k < barrido.recibidos; k++)
            printf("%5d  %10ld  %8.2f\n", barrido.pts[k].i, barrido.pts[k].freq, barrido.pts[k].db);
    }
    printf("recibidos: %d/%ld\n", barrido.recibidos, barrido.puntos);
}

/* SWEEP empieza un barrido nuevo y POINT agrega un punto */
static int procesar_datos(const char *linea)
{
    long fi, ff, p, t, freq;
    int i;
    double db;
    struct punto *pt;

    if (sscanf(linea, "SWEEP frec_inicio=%ld frec_final=%ld puntos=%ld tiempo=%ld",
               &fi, &ff, &p, &t) == 4 &&
        p >= 1 && p <= MAX_PUNTOS)
    {
        barrido.frec_inicio = fi;
        barrido.frec_final = ff;
        barrido.puntos = p;
        barrido.tiempo = t;
        barrido.recibidos = 0;
        return 0;
    }

    if (sscanf(linea, "POINT i=%d freq=%ld db=%lf", &i, &freq, &db) == 3 && barrido.recibidos < barrido.puntos)
    {
        pt = &barrido.pts[barrido.recibidos];
        pt->i = i;
        pt->freq = freq;
        pt->db = db;
        barrido.recibidos++;
        if (i == barrido.puntos - 1)
            return 1;
    }
    return 0;
}

/* Hilo monitor: loop bloqueante de read(). Durante el
 * barrido no imprime nada, asi se puede escribir pause, resume o cancel
 * sin que se mezcle la salida. Al completarse muestra la tabla. */
static void *hilo_monitor(void *arg)
{
    char buf[MAX_LINE];   /* lo que trae cada read */
    char linea[MAX_LINE]; /* la linea que se va armando */
    size_t n = 0;         /* caracteres guardados en linea */
    ssize_t leidos;       /* bytes que trajo el read */

    while ((leidos = read(fd_data, buf, sizeof(buf))) > 0)
    {
        for (ssize_t k = 0; k < leidos; k++)
        {
            if (buf[k] != '\n')
            {
                if (n < sizeof(linea) - 1)
                    linea[n++] = buf[k];
            }
            else
            {
                linea[n] = '\0';
                n = 0;
                pthread_mutex_lock(&consola);
                if (procesar_datos(linea))
                {
                    printf("\n[barrido completo: %d puntos]\n", barrido.recibidos);
                    imprimir_tabla(1);
                    printf(PROMPT);
                    fflush(stdout);
                }
                pthread_mutex_unlock(&consola);
            }
        }
    }
    return NULL;
}

/* ===================== comandos al ESP32 ===================== */

/* Espera la respuesta del ESP32 en /dev/esp32link_cmd. Si no hay nada se espera 10 ms y
 * se reintenta, hasta 50 veces (500 ms). Devuelve 0 si llego una linea completa y -1 si no. */
static int esperar_respuesta(char *resp, size_t tam)
{
    ssize_t n;

    for (int intentos = 0; intentos < 50; intentos++)
    {
        if ((n = read(fd_cmd, resp, tam - 1)) > 0)
        {
            resp[n] = '\0';
            if (resp[n - 1] == '\n')
                resp[n - 1] = '\0';
            return 0;
        }
        usleep(10000);
    }
    return -1;
}

static void enviar_comando(const char *linea)
{
    char resp[MAX_LINE];
    int codigo;

    /* Descarta respuestas viejas: la primera linea que llegue despues
     * del write tiene que ser la de este comando */
    while (read(fd_cmd, resp, sizeof(resp)) > 0)
        ;

    if (write(fd_cmd, linea, strlen(linea)) < 0)
    {
        perror("write");
        return;
    }

    if (esperar_respuesta(resp, sizeof(resp)) < 0)
        printf("sin respuesta del ESP32\n");
    else if (sscanf(resp, "ERR %d", &codigo) == 1 && codigo >= 1 && codigo <= 5)
        printf("ERR %d: %s\n", codigo, errores[codigo]);
    else
        printf("%s\n", resp);
}

/* ===================== comandos locales ===================== */

static void uso(void)
{
    printf("Comandos del ESP32:\n"
           "  set <param> <valor>\n"
           "  get <param> | get config\n"
           "  start | pause | resume | cancel\n"
           "Comandos locales:\n"
           "  ayuda | estado | datos | salir\n"
           "Parametros:\n"
           "  frec_inicio 10 - 99999 Hz    frec_final 11 - 100000 Hz\n"
           "  puntos      2 - 512          tiempo     10 - 10000 ms (asentamiento)\n");
}

/* ===================== main ===================== */

int main(int argc, char *argv[])
{
    pthread_t monitor;
    char linea[MAX_LINE], cmd[16];

    (void)argv;
    if (argc > 1)
    {
        uso();
        return 0;
    }

    fd_cmd = open(ESP32_DEV_CMD, O_RDWR | O_NONBLOCK);
    if (fd_cmd < 0)
    {
        perror("open " ESP32_DEV_CMD);
        return 1;
    }
    fd_data = open(ESP32_DEV_DATA, O_RDONLY);
    if (fd_data < 0)
    {
        perror("open " ESP32_DEV_DATA);
        return 1;
    }

    printf("app_cli: escribir 'ayuda' para ver comandos\n" PROMPT);
    fflush(stdout);
    pthread_create(&monitor, NULL, hilo_monitor, NULL);

    while (fgets(linea, sizeof(linea), stdin))
    {
        if (!strncmp(linea, "salir", 5))
            break;

        pthread_mutex_lock(&consola);
        if (sscanf(linea, "%15s", cmd) != 1)
        {
            // linea vacia: no se manda nada
        }
        else if (!strcmp(cmd, "estado"))
        {
            imprimir_tabla(0);
        }
        else if (!strcmp(cmd, "datos"))
        {
            imprimir_tabla(1);
        }
        else if (!strcmp(cmd, "ayuda"))
        {
            uso();
        }
        else
        {
            enviar_comando(linea); // todo lo demas lo valida el ESP32
        }
        printf(PROMPT);
        fflush(stdout);
        pthread_mutex_unlock(&consola);
    }

    close(fd_data);
    close(fd_cmd);
    return 0;
}
