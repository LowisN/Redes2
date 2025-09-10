/* Garcia Mayorga Rodrigo
   Patlan Gualo Luis Eduardo
   6CV3
   Cliente con autenticación, menú, descarga de archivos y chat bidireccional
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <errno.h>

#define PORT 8080
#define BUFFER_SIZE 1024



/* Funciones auxiliares de comunicación */
int send_all(int sockfd, const void *buf, size_t len);
int send_line(int sockfd, const char *text);
ssize_t recv_line(int sockfd, char *out, size_t maxlen);
ssize_t read_n_bytes(int sockfd, void *buf, size_t n);

/* Funciones de lógica */
int autenticar(int sockfd, char *line, size_t len);
int mostrar_menu(int sockfd, char *line, size_t len);
void manejar_opcion(int sockfd, const char *opcion, char *line, size_t len);
void descargar_archivo(int sockfd, char *line, size_t len);
void chat_cliente(int sockfd, char *line, size_t len);


int main() {
    int sockfd;
    struct sockaddr_in servaddr;
    char line[BUFFER_SIZE];

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) { perror("socket"); exit(EXIT_FAILURE); }

    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(PORT);
    inet_pton(AF_INET, "127.0.0.1", &servaddr.sin_addr);

    if (connect(sockfd, (struct sockaddr*)&servaddr, sizeof(servaddr)) < 0) {
        perror("connect"); 
        close(sockfd); 
        exit(EXIT_FAILURE);
    }

    if (autenticar(sockfd, line, sizeof(line)) < 0) {
        printf("Error en autenticación.\n");
        close(sockfd);
        return 1;
    }

    while (1) {
        if (mostrar_menu(sockfd, line, sizeof(line)) < 0) break;
        if (!fgets(line, sizeof(line), stdin)) break;
        line[strcspn(line, "\r\n")] = '\0';
        send_line(sockfd, line);

        if (strcmp(line, "0") == 0) {
            if (recv_line(sockfd, line, sizeof(line)) > 0)
                printf("%s\n", line);
            break;
        }
        manejar_opcion(sockfd, line, line, sizeof(line));
    }

    close(sockfd);
    printf("Conexión cerrada.\n");
    return 0;
}



/* 
 * Enviar todos los bytes de un buffer
 * Parámetros: sockfd (socket), buf (datos), len (tamaño)
 * Retorno: 0 si todo OK, -1 si error*/
int send_all(int sockfd, const void *buf, size_t len) {
    size_t sent = 0;
    const char *p = buf;
    while (sent < len) {
        ssize_t n = send(sockfd, p + sent, len - sent, 0);
        if (n <= 0) return -1;
        sent += n;
    }
    return 0;
}

/* Enviar una línea terminada en '\n'*/
int send_line(int sockfd, const char *text) {
    if (send_all(sockfd, text, strlen(text)) < 0) return -1;
    if (send_all(sockfd, "\n", 1) < 0) return -1;
    return 0;
}

/* Recibir una línea sin incluir '\n'*/
ssize_t recv_line(int sockfd, char *out, size_t maxlen) {
    size_t idx = 0;
    char c;
    ssize_t r;
    while (idx < maxlen - 1) {
        r = read(sockfd, &c, 1);
        if (r == 1) {
            if (c == '\n') break;
            if (c == '\r') continue;
            out[idx++] = c;
        } else if (r == 0) {
            if (idx == 0) return 0;
            break;
        } else {
            if (errno == EINTR) continue;
            return -1;
        }
    }
    out[idx] = '\0';
    return (ssize_t)idx;
}

/* Leer n bytes exactos del socket*/
ssize_t read_n_bytes(int sockfd, void *buf, size_t n) {
    size_t nleft = n;
    char *p = buf;
    while (nleft > 0) {
        ssize_t r = read(sockfd, p, nleft);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        } else if (r == 0) break;
        nleft -= r;
        p += r;
    }
    return (ssize_t)(n - nleft);
}

/*Autenticación: pide usuario y contraseña
 * Retorno: 0 si OK, -1 si falla*/
int autenticar(int sockfd, char *line, size_t len) {
    if (recv_line(sockfd, line, len) <= 0) return -1;
    printf("%s\n", line);
    printf("Usuario: ");
    if (!fgets(line, len, stdin)) return -1;
    line[strcspn(line, "\r\n")] = '\0';
    send_line(sockfd, line);

    if (recv_line(sockfd, line, len) <= 0) return -1;
    printf("%s\n", line);
    printf("Contraseña: ");
    if (!fgets(line, len, stdin)) return -1;
    line[strcspn(line, "\r\n")] = '\0';
    send_line(sockfd, line);

    if (recv_line(sockfd, line, len) <= 0) return -1;
    printf("%s\n", line);
    return 0;
}

/*
 * Mostrar el menú hasta recibir "Elige opción:"
 * Retorno: 0 si OK, -1 si error/desconexión*/
int mostrar_menu(int sockfd, char *line, size_t len) {
    while (1) {
        ssize_t r = recv_line(sockfd, line, len);
        if (r <= 0) return -1;
        printf("%s\n", line);
        if (strcmp(line, "Elige opción:") == 0) break;
    }
    return 0;
}

/* Manejar la opción seleccionada del menú */
void manejar_opcion(int sockfd, const char *opcion, char *line, size_t len) {
    if (strcmp(opcion, "1") == 0) {
        descargar_archivo(sockfd, line, len);
    } else if (strcmp(opcion, "2") == 0) {
        chat_cliente(sockfd, line, len);
    } else {
        if (recv_line(sockfd, line, len) > 0)
            printf("%s\n", line);
    }
}

/*Descargar un archivo del servidor*/
void descargar_archivo(int sockfd, char *line, size_t len) {
    if (recv_line(sockfd, line, len) <= 0) { printf("Servidor desconectado.\n"); return; }
    printf("%s\n", line);
    if (!fgets(line, len, stdin)) return;
    line[strcspn(line, "\r\n")] = '\0';
    send_line(sockfd, line);

    if (recv_line(sockfd, line, len) <= 0) { printf("Servidor desconectado.\n"); return; }
    if (strncmp(line, "FILE_START ", 11) == 0) {
        long size = atol(line + 11);
        printf("Inicia descarga (%ld bytes)...\n", size);

        FILE *fp = fopen("archivo_recibido.txt", "wb");
        if (!fp) {
            perror("fopen");
            char tmp[BUFFER_SIZE];
            long remaining = size;
            while (remaining > 0) {
                ssize_t toread = (remaining > BUFFER_SIZE) ? BUFFER_SIZE : remaining;
                ssize_t got = read_n_bytes(sockfd, tmp, toread);
                if (got <= 0) break;
                remaining -= got;
            }
            recv_line(sockfd, line, len);
            return;
        }

        long remaining = size;
        while (remaining > 0) {
            ssize_t toread = (remaining > BUFFER_SIZE) ? BUFFER_SIZE : remaining;
            ssize_t got = read_n_bytes(sockfd, line, toread);
            if (got <= 0) break;
            fwrite(line, 1, got, fp);
            remaining -= got;
        }
        fclose(fp);
        recv_line(sockfd, line, len);
        printf("Archivo recibido y guardado como 'archivo_recibido.txt'.\n");
    } else {
        printf("%s\n", line);
    }
}

/* Chat cliente-servidor*/
void chat_cliente(int sockfd, char *line, size_t len) {
    if (recv_line(sockfd, line, len) <= 0) { printf("Servidor desconectado.\n"); return; }
    if (strcmp(line, "CHAT_START") != 0) {
        printf("%s\n", line);
        return;
    }
    printf("Entrando en chat. Escribe 'exit' para volver al menú.\n");

    while (1) {
        fd_set rset;
        FD_ZERO(&rset);
        FD_SET(STDIN_FILENO, &rset);
        FD_SET(sockfd, &rset);
        int maxfd = (sockfd > STDIN_FILENO) ? sockfd : STDIN_FILENO;

        if (select(maxfd + 1, &rset, NULL, NULL, NULL) < 0) {
            if (errno == EINTR) continue;
            perror("select cliente");
            return;
        }

        if (FD_ISSET(STDIN_FILENO, &rset)) {
            if (!fgets(line, len, stdin)) return;
            line[strcspn(line, "\r\n")] = '\0';
            send_line(sockfd, line);
            if (strcmp(line, "exit") == 0) {
                printf("Saliendo chat hacia menú...\n");
                break;
            }
        }

        if (FD_ISSET(sockfd, &rset)) {
            ssize_t r = recv_line(sockfd, line, len);
            if (r <= 0) { printf("Servidor desconectado durante chat.\n"); return; }
            if (strcmp(line, "exit") == 0) {
                printf("Servidor cerró el chat. Volviendo al menú...\n");
                break;
            }
            printf("Servidor: %s\n", line);
        }
    }
}
