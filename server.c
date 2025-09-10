/* Garcia Mayorga Rodrigo
   Parlante Gualo Luis Eduardo
   Servidor
   6CV3
   Servidor concurrente con autenticación, menú, envío de archivos y chat con MariaDB
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <errno.h>
#include <sys/select.h>
#include <mariadb/mysql.h>

#define PORT 8080
#define BUFFER_SIZE 1024
#define MAX_CLIENTS 10

// Configuración de la base de datos MariaDB
#define DB_HOST "localhost"
#define DB_USER "practica3_user"          // Usuario específico para la aplicación
#define DB_PASSWORD "password123"   // Contraseña del usuario de la aplicación
#define DB_NAME "practica3"     // Nombre de la base de datos

pthread_mutex_t stdin_mutex = PTHREAD_MUTEX_INITIALIZER;


int send_all(int sockfd, const void *buf, size_t len);
int send_line(int sockfd, const char *text);
ssize_t recv_line(int sockfd, char *out, size_t maxlen);
ssize_t read_n_bytes(int sockfd, void *buf, size_t n);
void *handle_client(void *arg);

// Funciones de base de datos
MYSQL* init_db_connection(void);
int authenticate_user(const char *username, const char *password);
void close_db_connection(MYSQL *conn);

/**FUNCIÓN PRINCIPAL*/
int main() {
    int server_fd;
    struct sockaddr_in servaddr;
    pthread_t tid;

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); exit(EXIT_FAILURE); }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = INADDR_ANY;
    servaddr.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr*)&servaddr, sizeof(servaddr)) < 0) {
        perror("bind");
        close(server_fd);
        exit(EXIT_FAILURE);
    }
    if (listen(server_fd, MAX_CLIENTS) < 0) {
        perror("listen");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    printf("Servidor esperando conexiones en el puerto %d...\n", PORT);
    printf("Autenticación habilitada con MariaDB\n");
    printf("Base de datos: %s\n", DB_NAME);

    while (1) {
        typedef struct {
            int sockfd;
            struct sockaddr_in addr;
        } client_info_t;

        client_info_t *cli = malloc(sizeof(client_info_t));
        if (!cli) { perror("malloc"); continue; }

        socklen_t len = sizeof(cli->addr);
        cli->sockfd = accept(server_fd, (struct sockaddr*)&cli->addr, &len);
        if (cli->sockfd < 0) { perror("accept"); free(cli); continue; }

        if (pthread_create(&tid, NULL, handle_client, cli) != 0) {
            perror("pthread_create");
            close(cli->sockfd);
            free(cli);
            continue;
        }
        pthread_detach(tid);
    }

    close(server_fd);
    return 0;
}




/* 
 * Enviar todos los bytes de un buffer
 * Parámetros: sockfd (socket), buf (datos), len (tamaño)
 * Retorno: 0 si OK, -1 en error */
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

/*
 * Enviar una línea terminada en '\n'*/
int send_line(int sockfd, const char *text) {
    size_t len = strlen(text);
    if (send_all(sockfd, text, len) < 0) return -1;
    if (send_all(sockfd, "\n", 1) < 0) return -1;
    return 0;
}

/* 
 * Recibir una línea (sin incluir '\n')
 * Retorno: >0 si OK, 0 si EOF, -1 si error*/
ssize_t recv_line(int sockfd, char *out, size_t maxlen) {
    size_t idx = 0;
    char c;
    ssize_t r;
    while (idx < maxlen - 1) {
        r = read(sockfd, &c, 1);
        if (r == 1) {
            if (c == '\n') break;
            if (c == '\r') continue; // ignorar CR
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

/* Leer exactamente n bytes desde el socket
 * Retorno: n si OK, menor si EOF, -1 en error */
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

/* Hilo que atiende a un cliente
 * Maneja: autenticación, menú, descarga y chat*/
void *handle_client(void *arg) {
    typedef struct {
        int sockfd;
        struct sockaddr_in addr;
    } client_info_t;

    client_info_t *cli = (client_info_t*)arg;
    char line[BUFFER_SIZE];

    printf("Nuevo cliente %s:%d\n",
           inet_ntoa(cli->addr.sin_addr),
           ntohs(cli->addr.sin_port));

    // pedir usuario
    if (send_line(cli->sockfd, "Por favor ingrese su ID de usuario:") < 0) goto cleanup;
    if (recv_line(cli->sockfd, line, sizeof(line)) <= 0) goto cleanup;
    char username[BUFFER_SIZE];
    strncpy(username, line, sizeof(username)-1);
    username[sizeof(username)-1] = '\0';

    // pedir contraseña
    if (send_line(cli->sockfd, "Ingrese su contraseña:") < 0) goto cleanup;
    if (recv_line(cli->sockfd, line, sizeof(line)) <= 0) goto cleanup;
    char password[BUFFER_SIZE];
    strncpy(password, line, sizeof(password)-1);
    password[sizeof(password)-1] = '\0';

    // Autenticar con la base de datos
    if (authenticate_user(username, password) != 1) {
        send_line(cli->sockfd, "Autenticación fallida. Usuario o contraseña incorrectos.");
        goto cleanup;
    }

    send_line(cli->sockfd, "Autenticación exitosa! Bienvenido.");
    printf("Cliente %s:%d autenticado correctamente como '%s'\n",
           inet_ntoa(cli->addr.sin_addr),
           ntohs(cli->addr.sin_port), username);

    // loop del menú
    while (1) {
        send_line(cli->sockfd, "¿Qué deseas hacer?");
        send_line(cli->sockfd, "1. Descargar archivo");
        send_line(cli->sockfd, "2. Iniciar chat");
        send_line(cli->sockfd, "0. Cerrar conexión");
        send_line(cli->sockfd, "Elige opción:");

        if (recv_line(cli->sockfd, line, sizeof(line)) <= 0) break;

        if (strcmp(line, "1") == 0) {
            // pedir nombre de archivo
            send_line(cli->sockfd, "Ingrese el nombre del archivo que desea descargar:");
            if (recv_line(cli->sockfd, line, sizeof(line)) <= 0) break;
            char filename[BUFFER_SIZE];
            strncpy(filename, line, sizeof(filename)-1);
            filename[sizeof(filename)-1] = '\0';

            FILE *f = fopen(filename, "rb");
            if (!f) {
                send_line(cli->sockfd, "Archivo no encontrado.");
            } else {
                fseek(f, 0, SEEK_END);
                long size = ftell(f);
                fseek(f, 0, SEEK_SET);

                char hdr[64];
                snprintf(hdr, sizeof(hdr), "FILE_START %ld", size);
                if (send_line(cli->sockfd, hdr) < 0) { fclose(f); break; }

                char buf[BUFFER_SIZE];
                size_t tosend;
                while ((tosend = fread(buf, 1, sizeof(buf), f)) > 0) {
                    if (send_all(cli->sockfd, buf, tosend) < 0) break;
                }
                fclose(f);
                send_line(cli->sockfd, "FILE_END");

                printf("Archivo '%s' enviado (%ld bytes) a %s:%d\n",
                       filename, size,
                       inet_ntoa(cli->addr.sin_addr),
                       ntohs(cli->addr.sin_port));
            }
        } else if (strcmp(line, "2") == 0) {
            if (send_line(cli->sockfd, "CHAT_START") < 0) break;
            printf("Chat iniciado con %s:%d (escribe 'exit' para terminar chat)\n",
                   inet_ntoa(cli->addr.sin_addr),
                   ntohs(cli->addr.sin_port));

            while (1) {
                fd_set rset;
                FD_ZERO(&rset);
                FD_SET(cli->sockfd, &rset);
                FD_SET(STDIN_FILENO, &rset);
                int maxfd = (cli->sockfd > STDIN_FILENO) ? cli->sockfd : STDIN_FILENO;

                int sel = select(maxfd + 1, &rset, NULL, NULL, NULL);
                if (sel < 0) {
                    if (errno == EINTR) continue;
                    perror("select chat servidor");
                    goto cleanup;
                }

                if (FD_ISSET(cli->sockfd, &rset)) {
                    ssize_t r = recv_line(cli->sockfd, line, sizeof(line));
                    if (r <= 0) {
                        printf("Cliente %s:%d desconectó durante chat\n",
                               inet_ntoa(cli->addr.sin_addr),
                               ntohs(cli->addr.sin_port));
                        goto cleanup;
                    }
                    printf("Cliente %s:%d: %s\n",
                           inet_ntoa(cli->addr.sin_addr),
                           ntohs(cli->addr.sin_port), line);
                    if (strcmp(line, "exit") == 0) {
                        send_line(cli->sockfd, "exit");
                        break;
                    }
                }

                if (FD_ISSET(STDIN_FILENO, &rset)) {
                    pthread_mutex_lock(&stdin_mutex);
                    if (fgets(line, sizeof(line), stdin) == NULL) {
                        pthread_mutex_unlock(&stdin_mutex);
                        send_line(cli->sockfd, "exit");
                        break;
                    }
                    line[strcspn(line, "\r\n")] = '\0';
                    pthread_mutex_unlock(&stdin_mutex);

                    if (strcmp(line, "exit") == 0) {
                        send_line(cli->sockfd, "exit");
                        break;
                    } else {
                        send_line(cli->sockfd, line);
                    }
                }
            }
            printf("Chat finalizado con %s:%d\n",
                   inet_ntoa(cli->addr.sin_addr),
                   ntohs(cli->addr.sin_port));
        } else if (strcmp(line, "0") == 0) {
            send_line(cli->sockfd, "Cerrando conexión. Hasta luego.");
            break;
        } else {
            send_line(cli->sockfd, "Opción no válida.");
        }
    }

cleanup:
    close(cli->sockfd);
    printf("Conexión cerrada con %s:%d\n",
           inet_ntoa(cli->addr.sin_addr),
           ntohs(cli->addr.sin_port));
    free(cli);
    return NULL;
}

/* Inicializar conexión a la base de datos MariaDB
 * Retorna: puntero a MYSQL si OK, NULL si error */
MYSQL* init_db_connection(void) {
    MYSQL *conn = mysql_init(NULL);
    if (conn == NULL) {
        fprintf(stderr, "mysql_init() failed\n");
        return NULL;
    }

    if (mysql_real_connect(conn, DB_HOST, DB_USER, DB_PASSWORD, DB_NAME, 0, NULL, 0) == NULL) {
        fprintf(stderr, "mysql_real_connect() failed: %s\n", mysql_error(conn));
        mysql_close(conn);
        return NULL;
    }

    return conn;
}

/* Cerrar conexión a la base de datos */
void close_db_connection(MYSQL *conn) {
    if (conn != NULL) {
        mysql_close(conn);
    }
}

/* Autenticar usuario contra la base de datos
 * Parámetros: username y password del usuario
 * Retorna: 1 si autenticación exitosa, 0 si falla, -1 si error de BD */
int authenticate_user(const char *username, const char *password) {
    MYSQL *conn = init_db_connection();
    if (conn == NULL) {
        return -1;
    }

    // Preparar consulta SQL para verificar usuario y contraseña
    char query[512];
    snprintf(query, sizeof(query), 
             "SELECT COUNT(*) FROM usuarios WHERE username = '%s' AND password = '%s'", 
             username, password);

    if (mysql_query(conn, query)) {
        fprintf(stderr, "Query failed: %s\n", mysql_error(conn));
        close_db_connection(conn);
        return -1;
    }

    MYSQL_RES *result = mysql_store_result(conn);
    if (result == NULL) {
        fprintf(stderr, "mysql_store_result() failed: %s\n", mysql_error(conn));
        close_db_connection(conn);
        return -1;
    }

    MYSQL_ROW row = mysql_fetch_row(result);
    int authenticated = 0;
    if (row != NULL && atoi(row[0]) > 0) {
        authenticated = 1;
    }

    mysql_free_result(result);
    close_db_connection(conn);
    return authenticated;
}
