/* Garcia Mayorga Rodrigo
   Patlan Gualo Luis Eduardo
   6CV3
   Cliente con autenticación, menú, descarga de archivos, chat bidireccional
   y sistema clave-valor (Reto 2) con manejo de timeouts y reintentos (Reto 3)
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <errno.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/buffer.h>

#define PORT 8080
#define BUFFER_SIZE 1024
#define MAX_RETRIES 3
#define RECV_TIMEOUT_SEC 5

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
void manejar_almacenamiento_clave_valor(int sockfd, char *line, size_t len);
void clave_valor_cliente(int sockfd, char *line, size_t len);

/* Funciones base64 para valores binarios (Reto 2) */
char* base64_encode(const unsigned char* input, int length);
unsigned char* base64_decode(const char* input, int* output_length);

int main() {
    int sockfd;
    struct sockaddr_in servaddr;
    char line[BUFFER_SIZE];

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) { perror("socket"); exit(EXIT_FAILURE); }

    // Configurar timeout de recepción (Reto 3)
    struct timeval timeout;
    timeout.tv_sec = RECV_TIMEOUT_SEC;
    timeout.tv_usec = 0;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

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
        printf("Opción: ");
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
 * Enviar todos los bytes de un buffer con reintentos (Reto 3)
 * Parámetros: sockfd (socket), buf (datos), len (tamaño)
 * Retorno: 0 si todo OK, -1 si error
 */
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
        retries = 0; // Reiniciar contador después de envío exitoso
    }
    
    return (sent == len) ? 0 : -1;
}

/* Enviar una línea terminada en '\n'*/
int send_line(int sockfd, const char *text) {
    if (send_all(sockfd, text, strlen(text)) < 0) return -1;
    if (send_all(sockfd, "\n", 1) < 0) return -1;
    return 0;
}

/* Recibir una línea sin incluir '\n' con timeout (Reto 3)*/
ssize_t recv_line(int sockfd, char *out, size_t maxlen) {
    size_t idx = 0;
    char c;
    ssize_t r;
    int retries = 0;
    
    while (idx < maxlen - 1 && retries < MAX_RETRIES) {
        r = read(sockfd, &c, 1);
        if (r == 1) {
            if (c == '\n') break;
            if (c == '\r') continue;
            out[idx++] = c;
            retries = 0; // Reiniciar contador después de lectura exitosa
        } else if (r == 0) {
            if (idx == 0) return 0;
            break;
        } else {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                retries++;
                usleep(100000); // Esperar 100ms antes de reintentar
                continue;
            }
            return -1;
        }
    }
    out[idx] = '\0';
    return (ssize_t)idx;
}

/* Leer n bytes exactos del socket con reintentos (Reto 3)*/
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
        retries = 0; // Reiniciar contador después de lectura exitosa
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


void manejar_opcion(int sockfd, const char *opcion, char *line, size_t len) {
    if (strcmp(opcion, "1") == 0) {
        descargar_archivo(sockfd, line, len);
    } else if (strcmp(opcion, "2") == 0) {
        chat_cliente(sockfd, line, len);
    } else if (strcmp(opcion, "3") == 0) {
        clave_valor_cliente(sockfd, line, len);
    } else {
        if (recv_line(sockfd, line, len) > 0)
            printf("%s\n", line);
    }
}


/*Descargar un archivo del servidor con reintentos (Reto 3)*/
void descargar_archivo(int sockfd, char *line, size_t len) {
    if (recv_line(sockfd, line, len) <= 0) { 
        printf("Servidor desconectado.\n"); 
        return; 
    }
    printf("%s\n", line);
    printf("Nombre del archivo: ");
    if (!fgets(line, len, stdin)) return;
    line[strcspn(line, "\r\n")] = '\0';
    send_line(sockfd, line);

    if (recv_line(sockfd, line, len) <= 0) { 
        printf("Servidor desconectado.\n"); 
        return; 
    }
    
    if (strncmp(line, "FILE_START ", 11) == 0) {
        long size = atol(line + 11);
        printf("Inicia descarga (%ld bytes)...\n", size);

        char filename[BUFFER_SIZE];
        snprintf(filename, sizeof(filename), "archivo_recibido_%ld.txt", time(NULL));
        
        FILE *fp = fopen(filename, "wb");
        if (!fp) {
            perror("fopen");
            // Descartar los datos si no podemos abrir el archivo
            long remaining = size;
            char tmp[BUFFER_SIZE];
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
        int retries = 0;
        while (remaining > 0 && retries < MAX_RETRIES) {
            ssize_t toread = (remaining > BUFFER_SIZE) ? BUFFER_SIZE : remaining;
            ssize_t got = read_n_bytes(sockfd, line, toread);
            if (got <= 0) {
                retries++;
                usleep(100000); // Esperar 100ms antes de reintentar
                continue;
            }
            fwrite(line, 1, got, fp);
            remaining -= got;
            retries = 0; // Reiniciar contador después de lectura exitosa
        }
        fclose(fp);
        
        if (retries >= MAX_RETRIES) {
            printf("ERROR: No se pudo completar la descarga después de múltiples intentos.\n");
            remove(filename); // Eliminar archivo incompleto
        } else {
            recv_line(sockfd, line, len); // Leer "FILE_END"
            printf("Archivo recibido y guardado como '%s'.\n", filename);
        }
    } else {
        printf("%s\n", line);
    }
}

/* Chat cliente-servidor*/
void chat_cliente(int sockfd, char *line, size_t len) {
    if (recv_line(sockfd, line, len) <= 0) { 
        printf("Servidor desconectado.\n"); 
        return; 
    }
    if (strcmp(line, "CHAT_START") != 0) {
        printf("%s\n", line);
        return;
    }
    
    // Mostrar comandos especiales recibidos del servidor
    printf("Entrando en chat. Comandos disponibles:\n");
    while (1) {
        if (recv_line(sockfd, line, len) <= 0) { 
            printf("Servidor desconectado.\n"); 
            return; 
        }
        if (strcmp(line, "Escribe tu mensaje:") == 0) {
            printf("%s\n", line);
            break;
        }
        printf("%s\n", line);
    }

    while (1) {
        fd_set rset;
        FD_ZERO(&rset);
        FD_SET(STDIN_FILENO, &rset);
        FD_SET(sockfd, &rset);
        int maxfd = (sockfd > STDIN_FILENO) ? sockfd : STDIN_FILENO;

        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        if (select(maxfd + 1, &rset, NULL, NULL, &tv) < 0) {
            if (errno == EINTR) continue;
            perror("select cliente");
            return;
        }

        if (FD_ISSET(STDIN_FILENO, &rset)) {
            if (!fgets(line, len, stdin)) return;
            line[strcspn(line, "\r\n")] = '\0';
            
            if (send_line(sockfd, line) < 0) {
                printf("Error al enviar mensaje.\n");
                continue;
            }
            
            if (strcmp(line, "/exit") == 0) {
                printf("Saliendo chat hacia menú...\n");
                break;
            }
        }

        if (FD_ISSET(sockfd, &rset)) {
            ssize_t r = recv_line(sockfd, line, len);
            if (r <= 0) { 
                printf("Servidor desconectado durante chat.\n"); 
                return; 
            }
            if (strcmp(line, "exit") == 0) {
                printf("Servidor cerró el chat. Volviendo al menú...\n");
                break;
            }
            printf("Servidor: %s\n", line);
        }
    }
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

/* Manejar el sistema de almacenamiento clave-valor (Reto 2) */
void manejar_almacenamiento_clave_valor(int sockfd, char *line, size_t len) {
    printf("Sistema de almacenamiento clave-valor\n");
    
    // Recibir y mostrar las instrucciones del servidor
    while (1) {
        if (recv_line(sockfd, line, len) <= 0) {
            printf("Servidor desconectado.\n");
            return;
        }
        
        if (strcmp(line, "Ingrese comando:") == 0) {
            printf("%s\n", line);
            break;
        }
        
        printf("%s\n", line);
    }
    
    while (1) {
        printf("Comando: ");
        if (!fgets(line, len, stdin)) break;
        line[strcspn(line, "\r\n")] = '\0';
        
        if (send_line(sockfd, line) < 0) {
            printf("Error al enviar comando.\n");
            continue;
        }
        
        if (strcmp(line, "EXIT") == 0) {
            printf("Volviendo al menú principal...\n");
            break;
        }
        
        // Procesar respuesta del servidor
        if (strncmp(line, "GET ", 4) == 0) {
            // Para el comando GET, manejar posibles respuestas binarias
            if (recv_line(sockfd, line, len) <= 0) {
                printf("Servidor desconectado.\n");
                return;
            }
            
            if (strncmp(line, "OK: Valor binario", 17) == 0) {
                printf("%s\n", line);
                
                // Esperar BINARY_START
                if (recv_line(sockfd, line, len) <= 0) {
                    printf("Servidor desconectado.\n");
                    return;
                }
                
                if (strcmp(line, "BINARY_START") == 0) {
                    // Leer datos binarios
                    unsigned char binary_data[BUFFER_SIZE];
                    ssize_t data_read = read_n_bytes(sockfd, binary_data, BUFFER_SIZE);
                    
                    if (data_read > 0) {
                        // Esperar BINARY_END
                        if (recv_line(sockfd, line, len) <= 0) {
                            printf("Servidor desconectado.\n");
                            return;
                        }
                        
                        if (strcmp(line, "BINARY_END") == 0) {
                            // Decodificar base64
                            int decoded_len;
                            unsigned char* decoded = base64_decode((const char*)binary_data, &decoded_len);
                            
                            if (decoded != NULL) {
                                printf("Datos binarios decodificados (%d bytes):\n", decoded_len);
                                // Guardar en archivo o mostrar según sea necesario
                                char filename[BUFFER_SIZE];
                                snprintf(filename, sizeof(filename), "binary_data_%ld.bin", time(NULL));
                                
                                FILE *fp = fopen(filename, "wb");
                                if (fp) {
                                    fwrite(decoded, 1, decoded_len, fp);
                                    fclose(fp);
                                    printf("Datos guardados en: %s\n", filename);
                                } else {
                                    perror("Error al guardar archivo");
                                }
                                
                                free(decoded);
                            } else {
                                printf("Error al decodificar datos base64.\n");
                            }
                        }
                    }
                }
            } else {
                printf("%s\n", line);
            }
        } else {
            // Para otros comandos, simplemente mostrar la respuesta
            if (recv_line(sockfd, line, len) <= 0) {
                printf("Servidor desconectado.\n");
                return;
            }
            printf("%s\n", line);
        }
    }
}

void clave_valor_cliente(int sockfd, char *line, size_t len) {
    // Mostrar instrucciones iniciales (leer hasta "Ingrese comando:")
    while (1) {
        if (recv_line(sockfd, line, len) <= 0) { printf("Servidor desconectado.\n"); return; }
        printf("%s\n", line);
        if (strcmp(line, "Ingrese comando:") == 0) break;
    }

    while (1) {
        printf("> ");
        if (!fgets(line, len, stdin)) return;
        line[strcspn(line, "\r\n")] = '\0';
        send_line(sockfd, line);

        // Recibir respuesta(s) del servidor
        while (1) {
            if (recv_line(sockfd, line, len) <= 0) { printf("Servidor desconectado.\n"); return; }
            // Si el servidor manda el prompt, termina el ciclo de respuesta
            if (strcmp(line, "Ingrese comando:") == 0) break;
            printf("%s\n", line);
            // Si el servidor manda el mensaje de salida, termina la función
            if (strstr(line, "Volviendo al menú principal") != NULL) return;
        }
        // Si el usuario escribió EXIT, salimos del ciclo
        if (strcasecmp(line, "EXIT") == 0) break;
    }
}