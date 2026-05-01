#include <stdio.h>
#include <winsock2.h>

#pragma comment(lib,"ws2_32.lib")

int main()
{
    WSADATA wsa;
    SOCKET s, new_socket;
    struct sockaddr_in server, client;
    char buffer[1024];
    FILE *fp;
    int c, bytes;

    WSAStartup(MAKEWORD(2,2),&wsa);

    s = socket(AF_INET, SOCK_STREAM, 0);

    server.sin_family = AF_INET;
    server.sin_addr.s_addr = INADDR_ANY;
    server.sin_port = htons(5001);

    bind(s,(struct sockaddr *)&server , sizeof(server));
    listen(s,3);

    printf("Waiting for connection...\n");
    c = sizeof(struct sockaddr_in);
    new_socket = accept(s,(struct sockaddr *)&client, &c);

    fp = fopen("received.txt","wb");

    while((bytes = recv(new_socket, buffer, sizeof(buffer), 0)) > 0)
        fwrite(buffer,1,bytes,fp);

    fclose(fp);
    closesocket(s);
    WSACleanup();

    printf("File received successfully\n");
    return 0;
}
