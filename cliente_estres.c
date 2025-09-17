#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <errno.h>

#define PORT 8080
#define BUFFER_SIZE 1024

typedef struct {
    int id;
    char username[20];
    char password[20];
} ClientArgs;

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

int send_line(int sockfd, const char *text) {
    if (send_all(sockfd, text, strlen(text)) < 0) return -1;
    if (send_all(sockfd, "\n", 1) < 0) return -1;
    return 0;
}

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

void* client_thread(void *arg) {
    ClientArgs *args = (ClientArgs*)arg;
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in servaddr = {0};
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(PORT);
    inet_pton(AF_INET, "127.0.0.1", &servaddr.sin_addr);

    if (connect(sockfd, (struct sockaddr*)&servaddr, sizeof(servaddr)) < 0) {
        perror("connect");
        free(args);
        return NULL;
    }

    char buffer[BUFFER_SIZE];
    
    // Autenticación
    recv_line(sockfd, buffer, sizeof(buffer)); // Username prompt
    send_line(sockfd, args->username);
    
    recv_line(sockfd, buffer, sizeof(buffer)); // Password prompt
    send_line(sockfd, args->password);
    
    recv_line(sockfd, buffer, sizeof(buffer)); // Auth result
    if (strstr(buffer, "exitosa") == NULL) {
        printf("Cliente %d: Autenticación fallida\n", args->id);
        close(sockfd);
        free(args);
        return NULL;
    }

    // Menú principal
    while (recv_line(sockfd, buffer, sizeof(buffer)) > 0) {
        if (strstr(buffer, "Elige opción:") != NULL) break;
    }
    
    // Seleccionar chat
    send_line(sockfd, "2");
    
    // Esperar inicio de chat
    while (recv_line(sockfd, buffer, sizeof(buffer)) > 0) {
        if (strstr(buffer, "Escribe tu mensaje:") != NULL) break;
    }
    
    // Enviar mensaje de prueba
    char message[50];
    snprintf(message, sizeof(message), "Mensaje de prueba del cliente %d", args->id);
    send_line(sockfd, message);
    
    // Salir del chat
    usleep(100000); // Esperar 100ms
    send_line(sockfd, "exit");
    
    // Volver al menú y salir
    while (recv_line(sockfd, buffer, sizeof(buffer)) > 0) {
        if (strstr(buffer, "Elige opción:") != NULL) break;
    }
    send_line(sockfd, "0");
    
    close(sockfd);
    printf("Cliente %d completado\n", args->id);
    free(args);
    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        printf("Uso: %s <número_de_clientes>\n", argv[0]);
        return 1;
    }

    int num_clients = atoi(argv[1]);
    if (num_clients < 1 || num_clients > 100) {
        printf("Número de clientes debe ser entre 1-100\n");
        return 1;
    }

    pthread_t threads[num_clients];
    
    for (int i = 0; i < num_clients; i++) {
        ClientArgs *args = malloc(sizeof(ClientArgs));
        args->id = i + 1;
        snprintf(args->username, sizeof(args->username), "usuario%d", (i % 50) + 1);
        snprintf(args->password, sizeof(args->password), "password%d", (i % 50) + 1);
        
        if (pthread_create(&threads[i], NULL, client_thread, args) != 0) {
            perror("pthread_create");
            free(args);
        }
        usleep(10000); // Pequeña espera entre conexiones
    }

    for (int i = 0; i < num_clients; i++) {
        pthread_join(threads[i], NULL);
    }

    printf("Prueba de estrés completada con %d clientes\n", num_clients);
    return 0;
}