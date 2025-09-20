/* Garcia Mayorga Rodrigo
   Patlan Gualo Luis Eduardo
   Servidor
   6CV3
   Servidor concurrente con autenticación, menú, envío de archivos y chat con MariaDB
   Modificado para Reto 2 y Reto 3
gcc server_mdb.c -o server -lpthread -lmariadb -lssl -lcrypto
instalar la libreria sudo apt-get install libmariadb-dev libssl-dev
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
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/buffer.h>
#include <sys/time.h>
#include <ctype.h> // Para tolower

#define PORT 8080
#define BUFFER_SIZE 1024
#define MAX_CLIENTS 60
#define THREAD_POOL_SIZE 10  // Tamaño configurable del pool de hilos
#define MAX_RETRIES 3        // Número máximo de reintentos
#define RECV_TIMEOUT_SEC 10   // Timeout en segundos para recepción (antes 5)

// Configuración de la base de datos MariaDB
#define DB_HOST "localhost"
#define DB_USER "practica3_user"
#define DB_PASSWORD "020718"
#define DB_NAME "practica3"

// Estructura para compartir información entre hilos
typedef struct {
    int sockfd;
    struct sockaddr_in addr;
    char username[BUFFER_SIZE];
    int in_chat;
} client_info_t;

// Variables globales para manejar clientes en chat
client_info_t *chat_clients[MAX_CLIENTS] = {NULL};
pthread_mutex_t chat_clients_mutex = PTHREAD_MUTEX_INITIALIZER;

// Pool de hilos
pthread_t thread_pool[THREAD_POOL_SIZE];
pthread_mutex_t thread_pool_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t thread_pool_cond = PTHREAD_COND_INITIALIZER;
client_info_t *client_queue[MAX_CLIENTS];
int queue_front = 0, queue_rear = 0, queue_count = 0;

// Funciones para el pool de hilos
void *thread_pool_worker(void *arg);
void enqueue_client(client_info_t *cli);
client_info_t *dequeue_client();

// Funciones base64 para valores binarios
char* base64_encode(const unsigned char* input, int length);
unsigned char* base64_decode(const char* input, int* output_length);

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
int execute_server_query(const char *query);

// Nuevas funciones para Reto 2
int db_set_key(const char *key, const char *value, int is_binary);
char* db_get_key(const char *key, int *is_binary);
int db_del_key(const char *key);
int db_list_keys(int sockfd);

// Funciones para manejar chat
void add_client_to_chat(client_info_t *cli);
void remove_client_from_chat(int sockfd);
void send_server_message(const char *message);

/**FUNCIÓN PRINCIPAL*/
int main() {
    int server_fd;
    struct sockaddr_in servaddr;

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); exit(EXIT_FAILURE); }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // Configurar timeout de recepción (Reto 3)
    struct timeval timeout;
    timeout.tv_sec = RECV_TIMEOUT_SEC;
    timeout.tv_usec = 0;
    setsockopt(server_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

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
    printf("Tamaño del pool de hilos: %d\n", THREAD_POOL_SIZE);
    printf("Timeout de recepción: %d segundos\n", RECV_TIMEOUT_SEC);
    
    // Inicializar pool de hilos (Reto 3)
    for (int i = 0; i < THREAD_POOL_SIZE; i++) {
        if (pthread_create(&thread_pool[i], NULL, thread_pool_worker, NULL) != 0) {
            perror("pthread_create para pool de hilos");
            exit(EXIT_FAILURE);
        }
    }

    // Crear hilo para interfaz de consultas de base de datos del servidor
    pthread_t db_tid;
    if (pthread_create(&db_tid, NULL, server_db_interface, NULL) != 0) {
        perror("pthread_create para interfaz de BD");
    } else {
        pthread_detach(db_tid);
        printf("Interfaz de consultas de base de datos del servidor iniciada.\n");
        printf("Escribe 'query' seguido de una consulta SQL para ejecutarla en el servidor.\n");
        printf("Escribe 'msg <mensaje>' para enviar un mensaje a todos los clientes en chat.\n");
        printf("Ejemplo: query SELECT * FROM usuarios;\n");
        printf("Ejemplo: msg Hola a todos desde el servidor!\n");
    }

    while (1) {
        client_info_t *cli = malloc(sizeof(client_info_t));
        if (!cli) { perror("malloc");
            usleep(10000);
            continue; }

        socklen_t len = sizeof(cli->addr);
        cli->sockfd = accept(server_fd, (struct sockaddr*)&cli->addr, &len);
        if (cli->sockfd < 0) { 
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // No hay clientes esperando, liberar y continuar
                free(cli);
                usleep(100000); // Esperar 100ms antes de reintentar
                continue;
            }
            else{
                perror("accept");
                free(cli);
                continue;

            }
        }
            
        
        cli->in_chat = 0;
        cli->username[0] = '\0';

        // Configurar timeout para el cliente también
        setsockopt(cli->sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

        // Encolar cliente en lugar de crear un hilo directamente (Reto 3)
        enqueue_client(cli);
    }

    close(server_fd);
    return 0;
}

// Trabajador del pool de hilos (Reto 3)
void *thread_pool_worker(void *arg) {
    while (1) {
        client_info_t *cli = dequeue_client();
        if (cli != NULL) {
            handle_client(cli);
        }
    }
    return NULL;
}

// Encolar cliente (Reto 3)
void enqueue_client(client_info_t *cli) {
    pthread_mutex_lock(&thread_pool_mutex);
    
    if (queue_count < MAX_CLIENTS) {
        client_queue[queue_rear] = cli;
        queue_rear = (queue_rear + 1) % MAX_CLIENTS;
        queue_count++;
        pthread_cond_signal(&thread_pool_cond);
    } else {
        fprintf(stderr, "Cola de clientes llena, rechazando conexión\n");
        close(cli->sockfd);
        free(cli);
    }
    
    pthread_mutex_unlock(&thread_pool_mutex);
}

// Desencolar cliente (Reto 3)
client_info_t *dequeue_client() {
    pthread_mutex_lock(&thread_pool_mutex);
    
    while (queue_count == 0) {
        pthread_cond_wait(&thread_pool_cond, &thread_pool_mutex);
    }
    
    client_info_t *cli = client_queue[queue_front];
    queue_front = (queue_front + 1) % MAX_CLIENTS;
    queue_count--;
    
    pthread_mutex_unlock(&thread_pool_mutex);
    return cli;
}

/* Codificación base64 para valores binarios (Reto 2) */
char* base64_encode(const unsigned char* input, int length) {
    BIO *bmem, *b64;
    BUF_MEM *bptr;

    b64 = BIO_new(BIO_f_base64());
    bmem = BIO_new(BIO_s_mem());
    b64 = BIO_push(b64, bmem);
    BIO_write(b64, input, length);
    BIO_flush(b64);
    BIO_get_mem_ptr(b64, &bptr);

    char *buff = (char *)malloc(bptr->length + 1);
    memcpy(buff, bptr->data, bptr->length);
    buff[bptr->length] = 0;

    BIO_free_all(b64);
    return buff;
}

/* Decodificación base64 para valores binarios (Reto 2) */
unsigned char* base64_decode(const char* input, int* output_length) {
    BIO *b64, *bmem;
    size_t length = strlen(input);
    unsigned char *buffer = (unsigned char *)malloc(length);
    memset(buffer, 0, length);

    b64 = BIO_new(BIO_f_base64());
    bmem = BIO_new_mem_buf((void*)input, length);
    bmem = BIO_push(b64, bmem);
    *output_length = BIO_read(bmem, buffer, length);

    BIO_free_all(bmem);
    return buffer;
}

/* Enviar todos los bytes de un buffer con reintentos (Reto 3) */
int send_all(int sockfd, const void *buf, size_t len) {
    size_t sent = 0;
    const char *p = buf;
    int retries = 0;
    
    while (sent < len && retries < MAX_RETRIES) {
        ssize_t n = send(sockfd, p + sent, len - sent, 0);
        if (n <= 0) {
            retries++;
            usleep(100000); // Esperar 100ms antes de reintentar
            continue;
        }
        sent += n;
        retries = 0; // Reiniciar contador de reintentos después de un envío exitoso
    }
    
    return (sent == len) ? 0 : -1;
}

/* Enviar una línea terminada en '\n' */
int send_line(int sockfd, const char *text) {
    size_t len = strlen(text);
    if (send_all(sockfd, text, len) < 0) return -1;
    if (send_all(sockfd, "\n", 1) < 0) return -1;
    return 0;
}

/* Recibir una línea (sin incluir '\n') con timeout */
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
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                // Timeout, retornar lo que se haya leído hasta ahora
                break;
            }
            return -1;
        }
    }
    out[idx] = '\0';
    return (ssize_t)idx;
}

/* Leer exactamente n bytes desde el socket con reintentos (Reto 3) */
ssize_t read_n_bytes(int sockfd, void *buf, size_t n) {
    size_t nleft = n;
    char *p = buf;
    int retries = 0;
    
    while (nleft > 0 && retries < MAX_RETRIES) {
        ssize_t r = read(sockfd, p, nleft);
        if (r < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                retries++;
                usleep(100000); // Esperar 100ms antes de reintentar
                continue;
            }
            return -1;
        } else if (r == 0) {
            break;
        }
        nleft -= r;
        p += r;
        retries = 0; // Reiniciar contador de reintentos después de una lectura exitosa
    }
    return (ssize_t)(n - nleft);
}

/* Inicializar conexión a la base de datos MariaDB */
MYSQL* init_db_connection(void) {
    MYSQL *conn = mysql_init(NULL);
    if (conn == NULL) {
        fprintf(stderr, "mysql_init() failed\n");
        return NULL;
    }

    if (mysql_real_connect(conn, DB_HOST, DB_USER, DB_PASSWORD, DB_NAME, 0, NULL, CLIENT_FOUND_ROWS) == NULL) {
        fprintf(stderr, "mysql_real_connect() failed: %s\n", mysql_error(conn));
        mysql_close(conn);
        return NULL;
    }

    mysql_set_character_set(conn, "utf8mb4");
    
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
int store_message(const char *username, const char *message) {
    MYSQL *conn = init_db_connection();
    if (conn == NULL) {
        return -1;
    }

    // Escapar los datos para prevenir SQL injection
    char escaped_username[100];
    char escaped_message[2048];
    mysql_real_escape_string(conn, escaped_username, username, strlen(username));
    mysql_real_escape_string(conn, escaped_message, message, strlen(message));

    char query[4096];
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

/* Ejecutar consulta desde el servidor */
int execute_server_query(const char *query) {
    MYSQL *conn = init_db_connection();
    if (conn == NULL) {
        printf("Error: No se pudo conectar to the database\n");
        return -1;
    }

    printf("Ejecutando consulta: %s\n", query);

    if (mysql_query(conn, query)) {
        printf("Error en la consulta: %s\n", mysql_error(conn));
        close_db_connection(conn);
        return -1;
    }

    MYSQL_RES *result = mysql_store_result(conn);
    if (result == NULL) {
        if (mysql_field_count(conn) == 0) {
            int affected_rows = mysql_affected_rows(conn);
            printf("Consulta ejecutada. Filas afectadas: %d\n", affected_rows);
        } else {
            printf("Error: No se pudieron obtener los resultados\n");
        }
    } else {
        int num_fields = mysql_num_fields(result);
        MYSQL_FIELD *fields = mysql_fetch_fields(result);
        
        printf("\n");
        for (int i = 0; i < num_fields; i++) {
            printf("%-20s", fields[i].name);
            if (i < num_fields - 1) {
                printf(" | ");
            }
        }
        printf("\n");
        
        for (int i = 0; i < num_fields; i++) {
            for (int j = 0; j < 20; j++) printf("-");
            if (i < num_fields - 1) {
                printf("-+-");
            }
        }
        printf("\n");
        
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

/* Almacenar clave-valor en la base de datos (Reto 2) */
int db_set_key(const char *key, const char *value, int is_binary) {
    MYSQL *conn = init_db_connection();
    if (conn == NULL) {
        return -1;
    }

    char escaped_key[512];
    char escaped_value[8192];
    mysql_real_escape_string(conn, escaped_key, key, strlen(key));
    mysql_real_escape_string(conn, escaped_value, value, strlen(value));

    char query[16384];
    snprintf(query, sizeof(query),
             "INSERT INTO key_value_store (clave, valor, es_binario) "
             "VALUES ('%s', '%s', %d) "
             "ON DUPLICATE KEY UPDATE valor='%s', es_binario=%d, timestamp=NOW()",
             escaped_key, escaped_value, is_binary, escaped_value, is_binary);

    int success = 0;
    if (mysql_query(conn, query)) {
        fprintf(stderr, "Query failed: %s\n", mysql_error(conn));
    } else {
        success = (mysql_affected_rows(conn) > 0);
    }

    close_db_connection(conn);
    return success;
}

/* Obtener valor por clave desde la base de datos (Reto 2) */
char* db_get_key(const char *key, int *is_binary) {
    MYSQL *conn = init_db_connection();
    if (conn == NULL) {
        return NULL;
    }

    char escaped_key[256];
    mysql_real_escape_string(conn, escaped_key, key, strlen(key));

    char query[512];
    snprintf(query, sizeof(query),
             "SELECT valor, es_binario FROM key_value_store WHERE clave='%s'",
             escaped_key);

    if (mysql_query(conn, query)) {
        fprintf(stderr, "Query failed: %s\n", mysql_error(conn));
        close_db_connection(conn);
        return NULL;
    }

    MYSQL_RES *result = mysql_store_result(conn);
    if (result == NULL) {
        fprintf(stderr, "mysql_store_result() failed: %s\n", mysql_error(conn));
        close_db_connection(conn);
        return NULL;
    }

    char *value = NULL;
    MYSQL_ROW row = mysql_fetch_row(result);
    if (row != NULL) {
        value = strdup(row[0]);
        *is_binary = (row[1] != NULL) ? atoi(row[1]) : 0;
    }

    // Incrementar contador de accesos
    char update_query[512];
    snprintf(update_query, sizeof(update_query),
             "UPDATE key_value_store SET contador_accesos = contador_accesos + 1 WHERE clave='%s'",
             escaped_key);
    mysql_query(conn, update_query);

    mysql_free_result(result);
    close_db_connection(conn);
    return value;
}

/* Eliminar clave de la base de datos (Reto 2) */
int db_del_key(const char *key) {
    MYSQL *conn = init_db_connection();
    if (conn == NULL) {
        return -1;
    }

    char escaped_key[256];
    mysql_real_escape_string(conn, escaped_key, key, strlen(key));

    char query[512];
    snprintf(query, sizeof(query),
             "DELETE FROM key_value_store WHERE clave='%s'",
             escaped_key);

    if (mysql_query(conn, query)) {
        fprintf(stderr, "Query failed: %s\n", mysql_error(conn));
        close_db_connection(conn);
        return -1;
    }

    close_db_connection(conn);
    return mysql_affected_rows(conn);
}

/* Listar todas las claves almacenadas (Reto 2) */
int db_list_keys(int sockfd) {
    MYSQL *conn = init_db_connection();
    if (conn == NULL) {
        return -1;
    }

    char query[512] = "SELECT clave FROM key_value_store ORDER BY clave";

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

    send_line(sockfd, "=== CLAVES ALMACENADAS ===");
    
    MYSQL_ROW row;
    int count = 0;
    char response[BUFFER_SIZE];
    while ((row = mysql_fetch_row(result))) {
        if (count >= 50) { // Limitar a 50 claves para no saturar
            snprintf(response, sizeof(response), "... (mostrando %d de %d claves)", count, (int)mysql_num_rows(result));
            send_line(sockfd, response);
            break;
        }
        
        snprintf(response, sizeof(response), "%d. %s", ++count, row[0]);
        send_line(sockfd, response);
    }

    if (count == 0) {
        send_line(sockfd, "No hay claves almacenadas.");
    }

    mysql_free_result(result);
    close_db_connection(conn);
    return 0;
}

/* Añadir cliente a la lista de chat */
void add_client_to_chat(client_info_t *cli) {
    pthread_mutex_lock(&chat_clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (chat_clients[i] == NULL) {
            chat_clients[i] = cli;
            cli->in_chat = 1;
            break;
        }
    }
    pthread_mutex_unlock(&chat_clients_mutex);
}

/* Remover cliente de la lista de chat */
void remove_client_from_chat(int sockfd) {
    pthread_mutex_lock(&chat_clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (chat_clients[i] != NULL && chat_clients[i]->sockfd == sockfd) {
            chat_clients[i]->in_chat = 0;
            chat_clients[i] = NULL;
            break;
        }
    }
    pthread_mutex_unlock(&chat_clients_mutex);
}

/* Enviar mensaje del servidor a todos los clientes */
void send_server_message(const char *message) {
    char formatted_message[BUFFER_SIZE * 2];
    snprintf(formatted_message, sizeof(formatted_message), "[SERVIDOR]: %s", message);
    
    pthread_mutex_lock(&chat_clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (chat_clients[i] != NULL && chat_clients[i]->in_chat) {
            send_line(chat_clients[i]->sockfd, formatted_message);
        }
    }
    pthread_mutex_unlock(&chat_clients_mutex);
}

/* Interfaz para consultas de base de datos desde el servidor */
void *server_db_interface(void *arg) {
    char input[BUFFER_SIZE];
    
    while (1) {
        if (fgets(input, sizeof(input), stdin) == NULL) {
            continue;
        }
        
        input[strcspn(input, "\n")] = 0;
        
        if (strncmp(input, "query ", 6) == 0) {
            char *query = input + 6;
            execute_server_query(query);
        } else if (strncmp(input, "msg ", 4) == 0) {
            char *message = input + 4;
            send_server_message(message);
            printf("Mensaje enviado a todos los clientes: %s\n", message);
        } else if (strcmp(input, "exit") == 0) {
            printf("Saliendo del servidor...\n");
            exit(0);
        } else if (strlen(input) > 0) {
            printf("Comando no reconocido. Usa 'query' seguido de una consulta SQL o 'msg' seguido de un mensaje.\n");
        }
    }
    
    return NULL;
}

/* Hilo que atiende a un cliente */
void *handle_client(void *arg) {
    client_info_t *cli = (client_info_t*)arg;
    char line[BUFFER_SIZE];

    printf("Nuevo cliente %s:%d\n",
           inet_ntoa(cli->addr.sin_addr),
           ntohs(cli->addr.sin_port));

    // pedir username
    if (send_line(cli->sockfd, "Por favor ingrese su nombre de usuario:") < 0) goto cleanup;
    if (recv_line(cli->sockfd, line, sizeof(line)) <= 0) goto cleanup;
    strncpy(cli->username, line, sizeof(cli->username)-1);
    cli->username[sizeof(cli->username)-1] = '\0';

    // pedir contraseña
    if (send_line(cli->sockfd, "Ingrese su contraseña:") < 0) goto cleanup;
    if (recv_line(cli->sockfd, line, sizeof(line)) <= 0) goto cleanup;
    char password[BUFFER_SIZE];
    strncpy(password, line, sizeof(password)-1);
    password[sizeof(password)-1] = '\0';

    // Autenticar con la base de datos
    int auth_result = authenticate_user(cli->username, password);
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
           ntohs(cli->addr.sin_port), cli->username);

    // DESACTIVAR TIMEOUT DESPUÉS DE AUTENTICAR
    struct timeval no_timeout;
    no_timeout.tv_sec = 0;
    no_timeout.tv_usec = 0;
    setsockopt(cli->sockfd, SOL_SOCKET, SO_RCVTIMEO, &no_timeout, sizeof(no_timeout));

    // loop del menú
    while (1) {
        send_line(cli->sockfd, "¿Qué deseas hacer?");
        send_line(cli->sockfd, "1. Descargar archivo");
        send_line(cli->sockfd, "2. Iniciar chat");
        send_line(cli->sockfd, "3. Almacenamiento clave-valor (Reto 2)");
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
                int retries = 0;
                while ((tosend = fread(buf, 1, sizeof(buf), f)) > 0 && retries < MAX_RETRIES) {
                    if (send_all(cli->sockfd, buf, tosend) < 0) {
                        retries++;
                        usleep(100000); // Esperar 100ms antes de reintentar
                        continue;
                    }
                    retries = 0; // Reiniciar contador después de envío exitoso
                }
                fclose(f);
                
                if (retries >= MAX_RETRIES) {
                    send_line(cli->sockfd, "ERROR: No se pudo enviar el archivo después de múltiples intentos.");
                } else {
                    send_line(cli->sockfd, "FILE_END");
                    printf("Archivo '%s' enviado (%ld bytes) a %s:%d\n",
                           filename, size,
                           inet_ntoa(cli->addr.sin_addr),
                           ntohs(cli->addr.sin_port));
                }
            }
        } else if (strcmp(line, "2") == 0) {
            if (send_line(cli->sockfd, "CHAT_START") < 0) break;
            printf("Chat iniciado con %s:%d (usuario: %s)\n",
                   inet_ntoa(cli->addr.sin_addr),
                   ntohs(cli->addr.sin_port), cli->username);
            
            // Añadir cliente a la lista de chat
            add_client_to_chat(cli);
            
            // Enviar mensaje de bienvenida al chat
            send_line(cli->sockfd, "Bienvenido al chat! Escribe '/help' para ver comandos disponibles.");
            send_line(cli->sockfd, "NOTA: Los mensajes solo son visibles para el servidor.");
            
            // Enviar opciones de chat mejorado
            send_line(cli->sockfd, "Comandos especiales:");
            send_line(cli->sockfd, "/help - Mostrar ayuda");
            send_line(cli->sockfd, "/history - Ver historial de mensajes");
            send_line(cli->sockfd, "/edit <id> <nuevo mensaje> - Editar mensaje");
            send_line(cli->sockfd, "/exit - Salir del chat");
            send_line(cli->sockfd, "Escribe tu mensaje:");

            while (1) {
                fd_set rset;
                FD_ZERO(&rset);
                FD_SET(cli->sockfd, &rset);
                int maxfd = cli->sockfd;

                struct timeval tv;
                tv.tv_sec = 1;
                tv.tv_usec = 0;

                int sel = select(maxfd + 1, &rset, NULL, NULL, &tv);
                if (sel < 0) {
                    if (errno == EINTR) continue;
                    perror("select chat servidor");
                    break;
                }

                if (FD_ISSET(cli->sockfd, &rset)) {
                    ssize_t r = recv_line(cli->sockfd, line, sizeof(line));
                    if (r <= 0) {
                        printf("Cliente %s:%d desconectó durante chat\n",
                               inet_ntoa(cli->addr.sin_addr),
                               ntohs(cli->addr.sin_port));
                        break;
                    }
                    
                    // Procesar comandos especiales
                    if (strncmp(line, "/history", 8) == 0) {
                        get_message_history(cli->sockfd, cli->username);
                    }
                    else if (strncmp(line, "/edit", 5) == 0) {
                        char *token = strtok(line + 6, " ");
                        if (token != NULL) {
                            int message_id = atoi(token);
                            char *new_message = strtok(NULL, "");
                            if (new_message != NULL) {
                                int result = modify_message(message_id, cli->username, new_message);
                                if (result > 0) {
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
                    else if (strcmp(line, "/exit") == 0) {
                        send_line(cli->sockfd, "Saliendo del chat...");
                        break;
                    }
                    else if (strcmp(line, "/help") == 0) {
                        send_line(cli->sockfd, "Comandos disponibles:");
                        send_line(cli->sockfd, "/help - Mostrar esta ayuda");
                        send_line(cli->sockfd, "/history - Ver historial de mensajes");
                        send_line(cli->sockfd, "/edit <id> <nuevo mensaje> - Editar mensaje");
                        send_line(cli->sockfd, "/exit - Salir del chat");
                    }
                    else if (strcmp(line, "exit") == 0) {
                        send_line(cli->sockfd, "exit");
                        break;
                    }
                    else {
                        // Almacenar mensaje normal en la base de datos
                        if (store_message(cli->username, line) == 1) {
                            printf("Mensaje de %s (%s:%d): %s (almacenado en BD)\n",
                                   cli->username,
                                   inet_ntoa(cli->addr.sin_addr),
                                   ntohs(cli->addr.sin_port), line);
                            // Confirmar al cliente que el mensaje fue recibido
                            send_line(cli->sockfd, "Mensaje recibido por el servidor.");
                        } else {
                            printf("Error al almacenar mensaje de %s (%s:%d): %s\n",
                                   cli->username,
                                   inet_ntoa(cli->addr.sin_addr),
                                   ntohs(cli->addr.sin_port), line);
                            send_line(cli->sockfd, "Error: No se pudo almacenar el mensaje");
                        }
                    }
                }
            }
            
            // Remover cliente de la lista de chat
            remove_client_from_chat(cli->sockfd);
            printf("Chat finalizado con %s:%d\n",
                   inet_ntoa(cli->addr.sin_addr),
                   ntohs(cli->addr.sin_port));
        } else if (strcmp(line, "3") == 0) {
            send_line(cli->sockfd, "SISTEMA CLAVE-VALOR");
            send_line(cli->sockfd, "Comandos disponibles:");
            send_line(cli->sockfd, "SET <clave> <valor> - Almacenar valor");
            send_line(cli->sockfd, "GET <clave> - Obtener valor");
            send_line(cli->sockfd, "DEL <clave> - Eliminar clave");
            send_line(cli->sockfd, "LIST - Listar todas las claves");
            send_line(cli->sockfd, "STATS - Ver estadísticas de operaciones"); // <-- AGREGA ESTA LÍNEA
            send_line(cli->sockfd, "EXIT - Volver al menú principal");

            while (1) {
                send_line(cli->sockfd, "Ingrese comando:");
                ssize_t r = recv_line(cli->sockfd, line, sizeof(line));
                if (r <= 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
                    break;
                }
                // Elimina espacios iniciales
                char *cmd = line;
                while (*cmd == ' ') cmd++;
                // Procesa comando insensible a mayúsculas
                if (starts_with_ignore_case(cmd, "SET ")) {
                    struct timeval t1, t2;
    gettimeofday(&t1, NULL);

    char *key = strtok(cmd + 4, " ");
    char *value = strtok(NULL, "");
    const char *operacion = "SET";
    const char *clave = key ? key : "";

    if (!key || !value) {
        send_line(cli->sockfd, "Uso: SET <clave> <valor>");
    } else {
        int is_binary = 0;
        if (strncmp(value, "base64:", 7) == 0) {
            is_binary = 1;
            value += 7;
        }
        if (db_set_key(key, value, is_binary)) {
            send_line(cli->sockfd, "OK: Valor almacenado correctamente");
        } else {
            send_line(cli->sockfd, "ERROR: No se pudo almacenar el valor");
        }
    }

    gettimeofday(&t2, NULL);
    int ms = (int)((t2.tv_sec - t1.tv_sec) * 1000 + (t2.tv_usec - t1.tv_usec) / 1000);

    MYSQL *log_conn = mysql_init(NULL);
    if (mysql_real_connect(log_conn, DB_HOST, DB_USER, DB_PASSWORD, DB_NAME, 0, NULL, 0)) {
        char log_query[1024];
        snprintf(log_query, sizeof(log_query),
            "INSERT INTO auditoria (usuario, operacion, clave, tiempo_respuesta_ms) VALUES ('%s', '%s', '%s', %d)",
            cli->username, operacion, clave, ms);
        mysql_query(log_conn, log_query);
        mysql_close(log_conn);
    } else {
        mysql_close(log_conn);
    }
                }
                else if (starts_with_ignore_case(cmd, "GET ")) {
                    struct timeval t1, t2;
    gettimeofday(&t1, NULL);

    char *key = strtok(cmd + 4, " ");
    const char *operacion = "GET";
    const char *clave = key ? key : "";

    if (!key) {
        send_line(cli->sockfd, "Uso: GET <clave>");
    } else {
        int is_binary = 0;
        char *value = db_get_key(key, &is_binary);
        if (value != NULL) {
            if (is_binary) {
                int decoded_length;
                unsigned char *decoded = base64_decode(value, &decoded_length);
                if (decoded != NULL) {
                    char response[BUFFER_SIZE];
                    snprintf(response, sizeof(response), "OK: Valor binario (%d bytes)", decoded_length);
                    send_line(cli->sockfd, response);
                    send_line(cli->sockfd, "BINARY_START");
                    send_all(cli->sockfd, decoded, decoded_length);
                    send_line(cli->sockfd, "BINARY_END");
                    free(decoded);
                } else {
                    send_line(cli->sockfd, "ERROR: No se pudo decodificar el valor binario");
                }
            } else {
                char response[BUFFER_SIZE];
                snprintf(response, sizeof(response), "OK: %s", value);
                send_line(cli->sockfd, response);
            }
            free(value);
        } else {
            send_line(cli->sockfd, "ERROR: Clave no encontrada");
        }
    }

    gettimeofday(&t2, NULL);
    int ms = (int)((t2.tv_sec - t1.tv_sec) * 1000 + (t2.tv_usec - t1.tv_usec) / 1000);

    MYSQL *log_conn = mysql_init(NULL);
    if (mysql_real_connect(log_conn, DB_HOST, DB_USER, DB_PASSWORD, DB_NAME, 0, NULL, 0)) {
        char log_query[1024];
        snprintf(log_query, sizeof(log_query),
            "INSERT INTO auditoria (usuario, operacion, clave, tiempo_respuesta_ms) VALUES ('%s', '%s', '%s', %d)",
            cli->username, operacion, clave, ms);
        mysql_query(log_conn, log_query);
        mysql_close(log_conn);
    } else {
        mysql_close(log_conn);
    }
                }
                else if (starts_with_ignore_case(cmd, "DEL ")) {
                    struct timeval t1, t2;
    gettimeofday(&t1, NULL);

    char *key = strtok(cmd + 4, " ");
    const char *operacion = "DEL";
    const char *clave = key ? key : "";

    if (!key) {
        send_line(cli->sockfd, "Uso: DEL <clave>");
    } else {
        int result = db_del_key(key);
        if (result > 0) {
            send_line(cli->sockfd, "OK: Clave eliminada correctamente");
        } else if (result == 0) {
            send_line(cli->sockfd, "ERROR: Clave no encontrada");
        } else {
            send_line(cli->sockfd, "ERROR: No se pudo eliminar la clave");
        }
    }

    gettimeofday(&t2, NULL);
    int ms = (int)((t2.tv_sec - t1.tv_sec) * 1000 + (t2.tv_usec - t1.tv_usec) / 1000);

    MYSQL *log_conn = mysql_init(NULL);
    if (mysql_real_connect(log_conn, DB_HOST, DB_USER, DB_PASSWORD, DB_NAME, 0, NULL, 0)) {
        char log_query[1024];
        snprintf(log_query, sizeof(log_query),
            "INSERT INTO auditoria (usuario, operacion, clave, tiempo_respuesta_ms) VALUES ('%s', '%s', '%s', %d)",
            cli->username, operacion, clave, ms);
        mysql_query(log_conn, log_query);
        mysql_close(log_conn);
    } else {
        mysql_close(log_conn);
    }
                }
                else if (strcasecmp(cmd, "LIST") == 0) {
                    struct timeval t1, t2;
    gettimeofday(&t1, NULL);

    db_list_keys(cli->sockfd);

    gettimeofday(&t2, NULL);
    int ms = (int)((t2.tv_sec - t1.tv_sec) * 1000 + (t2.tv_usec - t1.tv_usec) / 1000);

    MYSQL *log_conn = mysql_init(NULL);
    if (mysql_real_connect(log_conn, DB_HOST, DB_USER, DB_PASSWORD, DB_NAME, 0, NULL, 0)) {
        char log_query[1024];
        snprintf(log_query, sizeof(log_query),
            "INSERT INTO auditoria (usuario, operacion, clave, tiempo_respuesta_ms) VALUES ('%s', 'LIST', '', %d)",
            cli->username, ms);
        mysql_query(log_conn, log_query);
        mysql_close(log_conn);
    } else {
        mysql_close(log_conn);
    }
                }
                else if (strcasecmp(cmd, "EXIT") == 0) {
                    send_line(cli->sockfd, "Volviendo al menú principal...");
                    break;
                }
                else if (strcasecmp(cmd, "STATS") == 0) {
                    struct timeval t1, t2;
    gettimeofday(&t1, NULL);

    MYSQL *log_conn = mysql_init(NULL);
    if (!mysql_real_connect(log_conn, DB_HOST, DB_USER, DB_PASSWORD, DB_NAME, 0, NULL, 0)) {
        send_line(cli->sockfd, "Error al conectar con la base de auditoría.");
        mysql_close(log_conn);
    } else {
        if (mysql_query(log_conn, "SELECT COUNT(*) FROM auditoria") == 0) {
            MYSQL_RES *res = mysql_store_result(log_conn);
            MYSQL_ROW row;
            if ((row = mysql_fetch_row(res))) {
                char stats[128];
                snprintf(stats, sizeof(stats), "Operaciones registradas: %s", row[0]);
                send_line(cli->sockfd, stats);
            }
            mysql_free_result(res);
        } else {
            send_line(cli->sockfd, "Error al consultar auditoría.");
        }
        mysql_close(log_conn);
    }

    gettimeofday(&t2, NULL);
    int ms = (int)((t2.tv_sec - t1.tv_sec) * 1000 + (t2.tv_usec - t1.tv_usec) / 1000);

    // Registrar auditoría para STATS
    log_conn = mysql_init(NULL);
    if (mysql_real_connect(log_conn, DB_HOST, DB_USER, DB_PASSWORD, DB_NAME, 0, NULL, 0)) {
        char log_query[1024];
        snprintf(log_query, sizeof(log_query),
            "INSERT INTO auditoria (usuario, operacion, clave, tiempo_respuesta_ms) VALUES ('%s', 'STATS', '', %d)",
            cli->username, ms);
        mysql_query(log_conn, log_query);
        mysql_close(log_conn);
    } else {
        mysql_close(log_conn);
    }
                }
                else {
                    send_line(cli->sockfd, "Comando no reconocido. Use SET, GET, DEL, LIST o EXIT");
                }
                // Mueve el prompt aquí, después de procesar el comando
                // send_line(cli->sockfd, "Ingrese comando:"); // <-- QUITA ESTA LÍNEA DEL INICIO DEL WHILE
            }
        } else if (strcmp(line, "0") == 0) {
            send_line(cli->sockfd, "Cerrando conexión. Hasta luego.");
            break;
        } else {
            send_line(cli->sockfd, "Opción no válida.");
        }
    }

cleanup:
    // Asegurarse de remover el cliente de la lista de chat si está allí
    remove_client_from_chat(cli->sockfd);
    
    close(cli->sockfd);
    printf("Conexión cerrada con %s:%d\n",
           inet_ntoa(cli->addr.sin_addr),
           ntohs(cli->addr.sin_port));
    free(cli);
    return NULL;
}

int starts_with_ignore_case(const char *str, const char *prefix) {
    while (*prefix) {
        if (tolower((unsigned char)*str) != tolower((unsigned char)*prefix)) return 0;
        str++;
        prefix++;
    }
    return 1;
}