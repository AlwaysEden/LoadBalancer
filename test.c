#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#define MAX_LINES 100

struct server{
    char ip[12];
    uint16_t port;
};


int main() {
    int lineCount = 0;

    // txt 파일 열기
    FILE *file = fopen("input.txt", "r");
    if (file == NULL) {
        printf("Error: 파일을 열 수 없습니다.\n");
        return 1;
    }

    struct server server[10];
    // 파일에서 IP 주소와 포트 번호를 여러 줄 읽기
    char buffer[1024];
    while (fscanf(file, "%s %hu", server[lineCount].ip, &server[lineCount].port) == 2) {
        lineCount++;
        if (lineCount >= MAX_LINES) {
            printf("Error: 최대 라인 수를 초과하였습니다.\n");
            break;
        }
    }

    // 파일 닫기
    fclose(file);

    // 읽은 IP 주소와 포트 번호 출력
    for (int i = 0; i < lineCount; i++) {
        printf("IP: %s, port: %hu\n", server[i].ip, server[i].port);
    }

    return 0;
}
