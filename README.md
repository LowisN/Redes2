Para compilar servidor:
gcc -Wall -Wextra -std=c99 -pthread \
    -I/usr/include/mariadb -I/usr/include/mariadb/mysql \
    -o servidor server.c \
    -L/usr/lib/x86_64-linux-gnu/ -lmariadb

Para cliente:
gcc -Wall -Wextra -std=c99 -pthread -o cliente cliente.c
