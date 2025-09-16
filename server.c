/* Garcia Mayorga Rodrigo
   Patlan Gualo Luis Eduardo
   Servidor
   6CV3
   Servidor concurrente con autenticación, menú, envío de archivos y chat con MySQL
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <errno.h>
#include <sys/select.h>
#include <mysql/mysql.h>

#define PORT 8080
#define BUFFER_SIZE 1024
#define MAX_CLIENTS 10

// Configuración de la base de datos MySQL
#define DB_HOST "localhost"
#define DB_USER "practica3_user"     // Usuario específico para la aplicación
#define DB_PASSWORD "password123"    // Contraseña del usuario de la aplicación
#define DB_NAME "practica3"          // Nombre de la base de datos

pthread_mutex_t stdin_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t db_query_mutex = PTHREAD_MUTEX_INITIALIZER;

int send_all(int sockfd, const void *buf, size_t len);
int send_line(int sockfd, const char *text);
ssize_t recv_line(int sockfd, char *out, size_t maxlen);
ssize_t read_n_bytes(int sockfd, void *buf, size_t n);
void *handle_client(void *arg);
void *server_db_interface(void *arg);

// Funciones de base de datos
MYSQL* init_db_connection(void);
int authenticate_user(const char *username, const char *password);
void close_db_connection(MYSQL *conn);
int store_message(const char *username, const char *message);
int get_last_message_id(const char *username);
int modify_message(int message_id, const char *username, const char *new_message);
int get_message_history(int sockfd, const char *username);
int execute_server_query(const char *query); // Nueva función para consultas del servidor

/**FUNCIÓN PRINCIPAL*/
int main() {
    int server_fd;
    struct sockaddr_in servaddr;
    pthread_t tid, db_tid;

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
    printf("Autenticación habilitada con MySQL\n");
    printf("Base de datos: %s\n", DB_NAME);
    
    // Crear hilo para interfaz de consultas de base de datos del servidor
    if (pthread_create(&db_tid, NULL, server_db_interface, NULL) != 0) {
        perror("pthread_create para interfaz de BD");
    } else {
        pthread_detach(db_tid);
        printf("Interfaz de consultas de base de datos del servidor iniciada.\n");
        printf("Escribe 'query' seguido de una consulta SQL para ejecutarla en el servidor.\n");
        printf("Ejemplo: query SELECT * FROM usuarios;\n");
    }

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

/* Enviar todos los bytes de un buffer */
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

/* Enviar una línea terminada en '\n' */
int send_line(int sockfd, const char *text) {
    size_t len = strlen(text);
    if (send_all(sockfd, text, len) < 0) return -1;
    if (send_all(sockfd, "\n", 1) < 0) return -1;
    return 0;
}

/* Recibir una línea (sin incluir '\n') */
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

/* Leer exactamente n bytes desde el socket */
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

/* Inicializar conexión a la base de datos MySQL */
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

/* Autenticar usuario contra la base de datos */
int authenticate_user(const char *username, const char *password) {
    MYSQL *conn = init_db_connection();
    if (conn == NULL) {
        return -1;
    }

    // ESCAPAR LOS DATOS PARA PREVENIR SQL INJECTION
    char escaped_username[100];
    char escaped_password[100];
    mysql_real_escape_string(conn, escaped_username, username, strlen(username));
    mysql_real_escape_string(conn, escaped_password, password, strlen(password));

    char query[512];
    snprintf(query, sizeof(query),
             "SELECT COUNT(*) FROM usuarios WHERE username='%s' AND password='%s'",
             escaped_username, escaped_password);

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

/* Almacenar mensaje en la base de datos */
/* Almacenar mensaje en la base de datos */
int store_message(const char *username, const char *message) {
    MYSQL *conn = init_db_connection();
    if (conn == NULL) {
        return -1;
    }

    // Escapar los datos para prevenir SQL injection
    char escaped_username[100];
    char escaped_message[2048];  // Buffer aumentado para mensajes largos
    mysql_real_escape_string(conn, escaped_username, username, strlen(username));
    mysql_real_escape_string(conn, escaped_message, message, strlen(message));

    char query[4096];  // Buffer de consulta más grande
    snprintf(query, sizeof(query),
             "INSERT INTO mensajes (username, mensaje, timestamp) VALUES ('%s', '%s', NOW())",
             escaped_username, escaped_message);

    int success = 0;
    if (mysql_query(conn, query)) {
        fprintf(stderr, "Query failed: %s\n", mysql_error(conn));
    } else {
        success = (mysql_affected_rows(conn) > 0);
    }

    close_db_connection(conn);
    return success;
}

/* Obtener el último ID de mensaje insertado */
int get_last_message_id(const char *username) {
    MYSQL *conn = init_db_connection();
    if (conn == NULL) {
        return -1;
    }

    char escaped_username[100];
    mysql_real_escape_string(conn, escaped_username, username, strlen(username));

    char query[512];
    snprintf(query, sizeof(query),
             "SELECT id FROM mensajes WHERE username='%s' ORDER BY id DESC LIMIT 1",
             escaped_username);

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

    int message_id = -1;
    MYSQL_ROW row = mysql_fetch_row(result);
    if (row != NULL) {
        message_id = atoi(row[0]);
    }

    mysql_free_result(result);
    close_db_connection(conn);
    return message_id;
}

/* Modificar mensaje existente */
int modify_message(int message_id, const char *username, const char *new_message) {
    MYSQL *conn = init_db_connection();
    if (conn == NULL) {
        return -1;
    }

    char escaped_username[100];
    char escaped_message[1024];
    mysql_real_escape_string(conn, escaped_username, username, strlen(username));
    mysql_real_escape_string(conn, escaped_message, new_message, strlen(new_message));

    char query[2048];
    snprintf(query, sizeof(query),
             "UPDATE mensajes SET mensaje='%s', editado=1, timestamp_edicion=NOW() "
             "WHERE id=%d AND username='%s'",
             escaped_message, message_id, escaped_username);

    if (mysql_query(conn, query)) {
        fprintf(stderr, "Query failed: %s\n", mysql_error(conn));
        close_db_connection(conn);
        return -1;
    }

    close_db_connection(conn);
    return mysql_affected_rows(conn);
}

/* Obtener historial de mensajes */
int get_message_history(int sockfd, const char *username) {
    MYSQL *conn = init_db_connection();
    if (conn == NULL) {
        return -1;
    }

    char escaped_username[100];
    mysql_real_escape_string(conn, escaped_username, username, strlen(username));

    char query[512];
    snprintf(query, sizeof(query),
             "SELECT id, mensaje, timestamp, editado FROM mensajes "
             "WHERE username='%s' ORDER BY timestamp DESC LIMIT 10",
             escaped_username);

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

    send_line(sockfd, "=== HISTORIAL DE MENSAJES ===");
    
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(result))) {
        char history_line[2048];
        if (row[3] != NULL && atoi(row[3]) == 1) {
            snprintf(history_line, sizeof(history_line), 
                    "[ID: %s] (editado) %s - %s", row[0], row[1], row[2]);
        } else {
            snprintf(history_line, sizeof(history_line), 
                    "[ID: %s] %s - %s", row[0], row[1], row[2]);
        }
        send_line(sockfd, history_line);
    }

    mysql_free_result(result);
    close_db_connection(conn);
    return 0;
}

/* Ejecutar consulta desde el servidor (nueva función) */
int execute_server_query(const char *query) {
    MYSQL *conn = init_db_connection();
    if (conn == NULL) {
        printf("Error: No se pudo conectar a la base de datos\n");
        return -1;
    }

    printf("Ejecutando consulta: %s\n", query);

    // Ejecutar la consulta
    if (mysql_query(conn, query)) {
        printf("Error en la consulta: %s\n", mysql_error(conn));
        close_db_connection(conn);
        return -1;
    }

    MYSQL_RES *result = mysql_store_result(conn);
    if (result == NULL) {
        // Consulta que no devuelve resultados (INSERT, UPDATE, DELETE, etc.)
        if (mysql_field_count(conn) == 0) {
            int affected_rows = mysql_affected_rows(conn);
            printf("Consulta ejecutada. Filas afectadas: %d\n", affected_rows);
        } else {
            printf("Error: No se pudieron obtener los resultados\n");
        }
    } else {
        // Consulta que devuelve resultados (SELECT, SHOW, etc.)
        int num_fields = mysql_num_fields(result);
        MYSQL_FIELD *fields = mysql_fetch_fields(result);
        
        // Mostrar nombres de columnas
        printf("\n");
        for (int i = 0; i < num_fields; i++) {
            printf("%-20s", fields[i].name);
            if (i < num_fields - 1) {
                printf(" | ");
            }
        }
        printf("\n");
        
        // Mostrar separador
        for (int i = 0; i < num_fields; i++) {
            for (int j = 0; j < 20; j++) printf("-");
            if (i < num_fields - 1) {
                printf("-+-");
            }
        }
        printf("\n");
        
        // Mostrar filas de resultados
        MYSQL_ROW row;
        while ((row = mysql_fetch_row(result))) {
            for (int i = 0; i < num_fields; i++) {
                if (row[i] == NULL) {
                    printf("%-20s", "NULL");
                } else {
                    printf("%-20s", row[i]);
                }
                if (i < num_fields - 1) {
                    printf(" | ");
                }
            }
            printf("\n");
        }
        
        int num_rows = mysql_num_rows(result);
        printf("\nFilas devueltas: %d\n", num_rows);
        
        mysql_free_result(result);
    }
    
    close_db_connection(conn);
    return 0;
}

/* Interfaz para consultas de base de datos desde el servidor */
void *server_db_interface(void *arg) {
    char input[BUFFER_SIZE];
    
    while (1) {
        // Leer entrada del servidor
        if (fgets(input, sizeof(input), stdin) == NULL) {
            continue;
        }
        
        // Eliminar salto de línea
        input[strcspn(input, "\n")] = 0;
        
        // Verificar si es un comando de consulta
        if (strncmp(input, "query ", 6) == 0) {
            char *query = input + 6;
            
            // Ejecutar la consulta
            pthread_mutex_lock(&db_query_mutex);
            execute_server_query(query);
            pthread_mutex_unlock(&db_query_mutex);
        } else if (strcmp(input, "exit") == 0) {
            printf("Saliendo del servidor...\n");
            exit(0);
        } else if (strlen(input) > 0) {
            printf("Comando no reconocido. Usa 'query' seguido de una consulta SQL.\n");
        }
    }
    
    return NULL;
}

/* Hilo que atiende a un cliente */
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

    // pedir username
    if (send_line(cli->sockfd, "Por favor ingrese su nombre de usuario:") < 0) goto cleanup;
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
    int auth_result = authenticate_user(username, password);
    if (auth_result != 1) {
        if (auth_result == 0) {
            send_line(cli->sockfd, "Autenticación fallida. Usuario o contraseña incorrectos.");
        } else {
            send_line(cli->sockfd, "Error: Problema con la base de datos. Intente más tarde.");
        }
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
            
            // Enviar opciones de chat mejorado
            send_line(cli->sockfd, "Comandos especiales:");
            send_line(cli->sockfd, "/history - Ver historial de mensajes");
            send_line(cli->sockfd, "/edit <id> <nuevo mensaje> - Editar mensaje");
            send_line(cli->sockfd, "Escribe tu mensaje:");

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
                    
                    // Procesar comandos especiales
                    if (strncmp(line, "/history", 8) == 0) {
                        get_message_history(cli->sockfd, username);
                    }
                    else if (strncmp(line, "/edit", 5) == 0) {
                        // Parsear comando: /edit <id> <nuevo mensaje>
                        char *token = strtok(line + 6, " ");
                        if (token != NULL) {
                            int message_id = atoi(token);
                            char *new_message = strtok(NULL, "");
                            if (new_message != NULL) {
                                int result = modify_message(message_id, username, new_message);
                                if (result <= 0) {
                                    send_line(cli->sockfd, "Mensaje modificado correctamente");
                                } else {
                                    send_line(cli->sockfd, "Error: No se pudo modificar el mensaje");
                                }
                            } else {
                                send_line(cli->sockfd, "Uso: /edit <id> <nuevo mensaje>");
                            }
                        } else {
                            send_line(cli->sockfd, "Uso: /edit <id> <nuevo mensaje>");
                        }
                    }
                    else if (strcmp(line, "exit") == 0) {
                        send_line(cli->sockfd, "exit");
                        break;
                    }
                    else {
                        // Almacenar mensaje normal en la base de datos
                       if (store_message(username, line) == 1) {
    printf("Cliente %s:%d: %s (almacenado en BD)\n",
           inet_ntoa(cli->addr.sin_addr),
           ntohs(cli->addr.sin_port), line);
} else {
    printf("Cliente %s:%d: %s (error al almacenar)\n",
           inet_ntoa(cli->addr.sin_addr),
           ntohs(cli->addr.sin_port), line);
}
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
