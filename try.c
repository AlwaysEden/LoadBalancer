#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <stdint.h>
#include <fcntl.h>
#include <errno.h>

#define SERVER_IP "192.168.0.37"  // 서버 IP 주소
#define SERVER_PORT 5000// 서버 포트 번호
#define LB_IP "192.168.0.6"
#define LB_PORT_FOR_SERVER 9191 
#define LB_PORT_FOR_CLIENT 9090
#define BUF_SIZE 1500
#define EPOLL_SIZE 50


char clnt_ip[1024];
uint16_t clnt_port;
int flag = 0;
struct pseudo_header
{
        uint32_t srcIP;
        uint32_t desIP;
        char Reserved;
        char protocol;
        uint16_t tcp_length;
};

void setnonblockingmode(int fd)
{
        int flag=fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flag|O_NONBLOCK);
}

uint16_t checksum(uint8_t *buffer, int len){

        uint32_t sum = 0;
        uint16_t *p = (uint16_t*)buffer;

        for(int i = 0; i < len/2; i++){
                sum += p[i];
                if(sum > 0xFFFF){
                        sum = (sum & 0xFFFF) + 1;
                }
        }

        if(len%2 != 0){
                sum += ((uint8_t*)buffer)[len-1];
                if(sum > 0xFFFF){
                        sum = (sum & 0xFFFF) + 1;
                }
        }


        return ((uint16_t)~sum);
}

struct pseudo_header createPseudoHeader(struct iphdr *ip_header, uint16_t tcp_frame_len){
        
	struct pseudo_header p_header;
	memset(&p_header, 0, sizeof(struct pseudo_header));	
	p_header.srcIP = ip_header->saddr;
        p_header.desIP = ip_header->daddr; 
        p_header.Reserved = 0;
       	p_header.protocol = 6;
	p_header.tcp_length = htons(tcp_frame_len);

	return p_header;
}

int replaceIP(struct iphdr *ip_header, char* src, char* dst){
    	/*
	struct in_addr new_source_ip;
    	int check = inet_pton(AF_INET, src, &new_source_ip);
	if(check != 1){
		return 1;
	}
    	ip_header->saddr = new_source_ip.s_addr;
	*/
    	struct in_addr new_dest_ip;
    	int check = inet_pton(AF_INET, dst, &new_dest_ip);
	if(check != 1){
		return 1;
	}
    	ip_header->daddr = new_dest_ip.s_addr;
	return 0;
}

int replacePort(struct tcphdr *tcp_header, uint16_t src, uint16_t dst){
	//tcp_header->source = htons(src);
    	// Modify the destination port
    	tcp_header->dest = htons(dst);
}

int serverConnect(){
    int sock;
    struct sockaddr_in server_addr;
    char send_data = 0;
    char recv_data[1024];
    int bytes_received;

    // 소켓 생성
    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("Socket creation error");
        return -1;
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);

    // 서버 주소 변환
    if (inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr) <= 0) {
        perror("Invalid address/ Address not supported");
        return -1;
    }

    // 서버에 연결
    if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Connection Failed");
        return -1;
    }
    return sock;
}

int dst_setting(struct sockaddr_in *daddr, char* ip, uint16_t port){
	daddr->sin_family = AF_INET;
	daddr->sin_port = htons(port);
	if(inet_pton(AF_INET, ip, &daddr->sin_addr) != 1){
		perror("Destination IP and Port configuration failure");
		return 1;
	}
	return 0;
}	

int main() {
    int lb_serv_sock, lb_clnt_sock;
    struct sockaddr_in serv_adr, clnt_adr;
    socklen_t adr_sz;
    int str_len, i;
    char buffer[BUF_SIZE];

    struct epoll_event *ep_events;
    struct epoll_event event;
    int epfd, event_cnt;

    //Create a socket for the clients.
    lb_serv_sock=socket(AF_INET, SOCK_RAW, IPPROTO_TCP);
    if(lb_serv_sock == -1){
		perror("socket");
		exit(1);
    }

    //Create a socket for the servers.
    lb_clnt_sock = socket(AF_INET, SOCK_RAW, IPPROTO_TCP);
    if(lb_clnt_sock == -1){
		perror("socket");
		exit(1);
    }
    int one = 1;
    const int *val = &one;
    if(setsockopt(lb_clnt_sock, IPPROTO_IP, IP_HDRINCL, val, sizeof(one))== -1){
	    perror("setsockopt(IP_HDRINCL, 1)");
	    return 1;
    }
    if(setsockopt(lb_serv_sock, IPPROTO_IP, IP_HDRINCL, val, sizeof(one))== -1){
	    perror("setsockopt(IP_HDRINCL, 1)");
	    return 1;
    }

    epfd=epoll_create(EPOLL_SIZE);
    ep_events=malloc(sizeof(struct epoll_event)*EPOLL_SIZE);

    event.events=EPOLLIN;
    event.data.fd=lb_serv_sock;
    epoll_ctl(epfd, EPOLL_CTL_ADD, lb_serv_sock, &event);

    event.events=EPOLLIN;
    event.data.fd=lb_clnt_sock;
    epoll_ctl(epfd, EPOLL_CTL_ADD, lb_clnt_sock, &event);

    //Make a socket to connect to the servers.
    /*
    int serv_socket = serverConnect();
    if(serv_socket == -1){
	    fprintf(stderr,"ERROR: serverConnect()\n");
	    exit(1);
    }
    event.events=EPOLLIN;
    event.data.fd=serv_socket;
    epoll_ctl(epfd, EPOLL_CTL_ADD, serv_socket, &event);
    */
    uint16_t lb_port = 0;
    int packet_len = -1;
    while(1){
	event_cnt = epoll_wait(epfd, ep_events, EPOLL_SIZE, -1);
	if(event_cnt == -1){
		puts("epoll_wait() error");
		break;
	}
	for(i = 0; i < event_cnt; i++){
		if(ep_events[i].data.fd == lb_serv_sock){//Packet From Client
			printf("Receive the packet from the client first\n");
			packet_len = recvfrom(lb_serv_sock, buffer, BUF_SIZE, 0, NULL, NULL);//Only Read from LB port openned for the clients	
			memcpy(&lb_port, buffer+22, sizeof(lb_port));
			if(ntohs(lb_port) != LB_PORT_FOR_CLIENT) continue;
			//if(flag == 1) continue;	
			//flag = 1;

			printf("from clnt 1\n");	
    			struct iphdr *ip_header = (struct iphdr *)buffer;
			struct in_addr current_ip;
		       	current_ip.s_addr = ip_header->saddr;
			char *check = inet_ntop(AF_INET, &current_ip, clnt_ip, 1024);
			if(check == NULL){
				perror("inet_ntop");
				exit(1);
			}

			printf("from clnt 2\n");	
			if(replaceIP(ip_header, LB_IP, SERVER_IP)){
				fprintf(stderr,"ERROR: inet_pton\n");
				exit(1);	
			}		
			ip_header->check = checksum((uint8_t*)ip_header, sizeof(struct iphdr));
			printf("from clnt 3\n");	
			struct tcphdr *tcp_header = (struct tcphdr *)(buffer + (ip_header->ihl * 4));
			tcp_header->check = 0;
			clnt_port = ntohs(tcp_header->source);
			replacePort(tcp_header,LB_PORT_FOR_SERVER, SERVER_PORT); //TODO: you need to change the port later for TCP
			
			int tot_len = ntohs(ip_header->tot_len);
			uint16_t tcp_frame_size = tot_len - (ip_header->ihl*4);

      			struct pseudo_header p_header = createPseudoHeader(ip_header, tcp_frame_size);
			int psize = sizeof(struct pseudo_header) + tcp_frame_size;
			char *checksum_header = malloc(psize);
			memset(checksum_header, 0, psize);
			memcpy(checksum_header, &p_header, sizeof(struct pseudo_header));
			memcpy(checksum_header+sizeof(struct pseudo_header), buffer+(ip_header->ihl*4), tcp_frame_size);

			tcp_header->check = checksum((uint8_t*)checksum_header, psize);	
			free(checksum_header);
			printf("from clnt 4\n");	
			
			struct sockaddr_in daddr;	
			if(dst_setting(&daddr, SERVER_IP, SERVER_PORT)){
				fprintf(stderr,"ERROR: dst_setting\n");
				exit(1);
			}
			sendto(lb_clnt_sock, buffer, packet_len, 0, (struct sockaddr*)&daddr, sizeof(struct sockaddr));
			printf("from clnt 5\n");	
		}else if(ep_events[i].data.fd == lb_clnt_sock){//Packet From Server
		
			printf("Receive the packet from the client first\n");
			packet_len = recvfrom(lb_clnt_sock, buffer, BUF_SIZE, 0, NULL, NULL);//Only Read from LB port openned for the clients	
			memcpy(&lb_port, buffer+22, sizeof(lb_port));
			if(ntohs(lb_port) != LB_PORT_FOR_SERVER) continue;
			//flag = 0;
			printf("from serv 1\n");
    			struct iphdr *ip_header = (struct iphdr *)buffer;
			if(replaceIP(ip_header, LB_IP, clnt_ip)){
				exit(1);
			}
			printf("from serv 2\n");
			ip_header->check = checksum((uint8_t*)ip_header, sizeof(struct iphdr));
			struct tcphdr *tcp_header = (struct tcphdr *)(buffer + (ip_header->ihl * 4));
			tcp_header->check = 0;
			replacePort(tcp_header,LB_PORT_FOR_CLIENT, clnt_port); //TODO: you need to change the port later for TCP

			int tot_len = ntohs(ip_header->tot_len);
			uint16_t tcp_frame_size = tot_len - (ip_header->ihl*4);

			printf("from serv 3\n");
      			struct pseudo_header p_header = createPseudoHeader(ip_header, tcp_frame_size);
			int psize = sizeof(struct pseudo_header) + tcp_frame_size;
			char *checksum_header = malloc(psize);
			memset(checksum_header, 0, psize);
			memcpy(checksum_header, &p_header, sizeof(struct pseudo_header));
			memcpy(checksum_header+sizeof(struct pseudo_header), buffer+(ip_header->ihl*4), tcp_frame_size);

			tcp_header->check = checksum(checksum_header, psize);	
			free(checksum_header);
			printf("from serv 4\n");
			
			struct sockaddr_in daddr;	
			if(dst_setting(&daddr, clnt_ip, clnt_port)){
				fprintf(stderr,"ERROR: dst_setting\n");
				exit(1);
			}
			sendto(lb_serv_sock, buffer, packet_len, 0, (struct sockaddr*)&daddr, sizeof(struct sockaddr));
			printf("from serv 5\n");
		}
	}
    }
    return 0;
}

