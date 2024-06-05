#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

#define SERVER_IP "192.168.0.37"  // 서버 IP 주소
#define SERVER_PORT 6000// 서버 포트 번호

int main() {
    int sock;
    struct sockaddr_in server_addr;
    char send_data = 0;
    char recv_data[1024];
    int bytes_received;

    // 소켓 생성
    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("Socket creation error");
        return 1;
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);

    // 서버 주소 변환
    if (inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr) <= 0) {
        perror("Invalid address/ Address not supported");
        return 1;
    }

    // 서버에 연결
    if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Connection Failed");
        return 1;
    }

    // 서버에 데이터 전송
    if (send(sock, &send_data, sizeof(send_data), 0) < 0) {
        perror("Send failed");
        close(sock);
        return 1;
    }

    // 서버로부터 응답 받기
    if ((bytes_received = recv(sock, recv_data, sizeof(recv_data) - 1, 0)) < 0) {
        perror("Receive failed");
        close(sock);
        return 1;
    }

    // 응답 데이터 처리
    recv_data[bytes_received] = '\0';  // null-terminate the received data
    printf("Received from server: %s\n", recv_data);

    // 소켓 닫기
    close(sock);
    return 0;
}

