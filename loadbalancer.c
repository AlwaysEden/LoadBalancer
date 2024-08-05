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
#include <stdbool.h>
#include <time.h>



#define LB_IP "192.168.0.6"
#define LB_PORT_FOR_CLIENT 9090
#define BUF_SIZE 1500
#define EPOLL_SIZE 50
#define MAX_CONNECTION 5012 
#define MAX_SERVER_CONNECTION 65536 

char clnt_ip[1024];
uint16_t clnt_port;
int table_connection_cnt;
int serv_total_connection;
struct table{
	char clnt_IP[15];
	uint16_t clnt_PORT;
	char serv_IP[15];
	uint16_t serv_PORT;
	uint16_t lb_PORT;
	uint8_t syn_state;
	uint8_t fin_state;
	uint32_t syn_seqNum;
	uint32_t fin_seqNum;
};
struct table table[MAX_CONNECTION];
int table_current_idx;

struct serv_info{
	char serv_IP[15];
	uint16_t serv_PORT;
	uint16_t serv_TCP_PORT;
	bool disconnect;
	int tcp_sock;
	int connect_cnt;
	int resource_rate;
};

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

uint16_t checksum(uint8_t *buffer, int len){ //Checksum Calculation For Rawsocket

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

struct pseudo_header createPseudoHeader(struct iphdr *ip_header, uint16_t tcp_frame_len){//PseudoHeader For Checksum Calculation
        
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
    	
	struct in_addr new_source_ip;
    	int check = inet_pton(AF_INET, src, &new_source_ip);
	if(check != 1){
		return 1;
	}
    	ip_header->saddr = new_source_ip.s_addr;

    	struct in_addr new_dest_ip;
    	check = inet_pton(AF_INET, dst, &new_dest_ip);
	if(check != 1){
		return 1;
	}
    	ip_header->daddr = new_dest_ip.s_addr;
	ip_header->check = checksum((uint8_t*)ip_header, sizeof(struct iphdr));
	return 0;
}

int replacePort(struct tcphdr *tcp_header, uint16_t src, uint16_t dst){
	tcp_header->source = htons(src);
    	tcp_header->dest = htons(dst);
}

int serverConnect(char* server_ip, uint16_t server_port){
    int sock;
    struct sockaddr_in server_addr;
    char send_data = 0;
    char recv_data[1024];
    int bytes_received;

    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("Socket creation error");
        return -1;
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port);

    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) <= 0) {
        perror("Invalid address/ Address not supported");
        return -1;
    }

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

int lb_algorithm(uint8_t user_algo, struct serv_info *serv_info, int *RR_idx){
	
	int selected_serv;
	int current_idx_RR = ++(*RR_idx);
	int minimum_idx = 0;
	switch(user_algo){
		case 0://Round Robin
			if((current_idx_RR) >= serv_total_connection){
				 *RR_idx = 0;
			}
			for(int i = current_idx_RR; i < serv_total_connection; i++){//Checking Disconnect
				if(serv_info[i].disconnect == false) break;
				current_idx_RR = ++(*RR_idx);
			}
			selected_serv = *RR_idx;
			break;
		case 1://At Least
			int minimum_connection = serv_info[0].connect_cnt;
			for(int i = 1; i < serv_total_connection; i++){
				if((minimum_connection > serv_info[i].connect_cnt) && (serv_info[i].disconnect == false)){
					minimum_idx = i;
					minimum_connection = serv_info[i].connect_cnt;
				}
			}
			selected_serv = minimum_idx;
			break;
		case 2://Resource Based
			int minimum_resource = serv_info[0].resource_rate;
			for(int i = 1; i < serv_total_connection; i++){
				if((minimum_connection > serv_info[i].resource_rate) && (serv_info[i].disconnect == false)){
					minimum_idx = i;
					minimum_resource= serv_info[i].resource_rate;
				}
			}
			selected_serv = minimum_idx;
			break;
	}
	return selected_serv;
}

void makeTable(struct serv_info target_serv, uint16_t lb_PORT){//serv_info, serv_idx, clnt_addr
	strcpy(table[table_current_idx].clnt_IP, clnt_ip);
	table[table_current_idx].clnt_IP[strlen(clnt_ip)] = 0x0;
	table[table_current_idx].clnt_PORT = clnt_port;
	strcpy(table[table_current_idx].serv_IP, target_serv.serv_IP);
	table[table_current_idx].serv_IP[strlen(target_serv.serv_IP)] = 0x0;
	table[table_current_idx].serv_PORT = target_serv.serv_PORT;
	table[table_current_idx].lb_PORT = lb_PORT;
	table_current_idx++;
	if(table_current_idx >= MAX_CONNECTION) table_current_idx = 0;
	if(table_connection_cnt < MAX_CONNECTION) table_connection_cnt++;
}

void makePacket(char *buffer, struct iphdr *ip_header, struct tcphdr *tcp_header){
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
}

int main(int argc, char *argv[]) {
    int lb_serv_sock, lb_clnt_sock;
    struct sockaddr_in serv_adr, clnt_adr;
    socklen_t adr_sz;
    int str_len, i;
    char buffer[BUF_SIZE];
    int current_idx_RR = -1;
	
    struct epoll_event *ep_events;
    struct epoll_event event;
    int epfd, event_cnt;
	

    if(argc < 1){
	printf("%s [Algorithm_name: RR(Round Robin)/AL(At Least)/RB(Resoure-Based)]\n",argv[0]);
	exit(1);
    }
    srand(time(NULL));
    uint8_t user_algo;
    if(strcmp(argv[1], "RR") == 0){
	user_algo = 0;
    }else if(strcmp(argv[1], "AL") == 0){
	user_algo = 1;
    }else if(strcmp(argv[1], "RB") == 0){
	user_algo = 2;
    }
    printf("User select: %d\n",user_algo);

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

    
    //Read the server information from File.
    FILE *fp = fopen("server.txt", "r");
    if(fp == NULL){
	fprintf(stderr,"ERROR: Open server file\n");
	exit(1);
    }
    struct serv_info serv_info[MAX_SERVER_CONNECTION];
    while(fscanf(fp, "%s %hu %hu",serv_info[serv_total_connection].serv_IP, &serv_info[serv_total_connection].serv_PORT, &serv_info[serv_total_connection].serv_TCP_PORT) == 3){
	serv_total_connection++;
    }

    //Make a socket to connect to the TCP servers.
    for(int i = 0; i < serv_total_connection; i++){ 
    	serv_info[i].tcp_sock = serverConnect(serv_info[i].serv_IP, serv_info[i].serv_TCP_PORT);
	if(serv_info[i].tcp_sock == -1){
		fprintf(stderr, "ERROR: Check this server: %s %d\n",serv_info[i].serv_IP, serv_info[i].serv_PORT);
		serv_info[i].disconnect = true;
		continue;
	}
	serv_info[i].connect_cnt = rand()%10; //For test
	serv_info[i].disconnect = false;
    	event.events=EPOLLIN;
    	event.data.fd=serv_info[i].tcp_sock;
    	epoll_ctl(epfd, EPOLL_CTL_ADD, serv_info[i].tcp_sock, &event);
    }

    
    uint16_t temp_port = 0; //For Checking the packet port. Temperory variable.
    int packet_len = -1;
    while(1){
	event_cnt = epoll_wait(epfd, ep_events, EPOLL_SIZE, -1);
	if(event_cnt == -1){
		puts("epoll_wait() error");
		break;
	}
	for(i = 0; i < event_cnt; i++){
		if(ep_events[i].data.fd == lb_serv_sock){//Packet From Client
			packet_len = recvfrom(lb_serv_sock, buffer, BUF_SIZE, 0, NULL, NULL);//Only Read from LB port openned for the clients	
			memcpy(&temp_port, buffer+22, sizeof(temp_port));
			temp_port = ntohs(temp_port);

			if(temp_port != LB_PORT_FOR_CLIENT) continue; //Distinguish the client port from many ports.
			
    			struct iphdr *ip_header = (struct iphdr *)buffer;
			struct tcphdr *tcp_header = (struct tcphdr *)(buffer + (ip_header->ihl * 4));

			struct in_addr current_ip;
			current_ip.s_addr = ip_header->saddr;
			char *check = inet_ntop(AF_INET, &current_ip, clnt_ip, 1024);
			if(check == NULL){
				perror("inet_ntop");
				exit(1);
			}
			
			int already_connected = 0;
			int current_table_idx = 0;
			for(int i = 0; i < table_connection_cnt; i++){ //Looking for whether it exists nor.
				if( (strcmp(table[i].clnt_IP, clnt_ip) == 0) && (table[i].clnt_PORT == ntohs(tcp_header->source)) ){
				       	already_connected= 1;
				       	current_table_idx = i;
				       	break;
				} 
			}	
			if(tcp_header->syn == 1){
				if(already_connected == 1) continue;
				if(user_algo == 1){ //AL mode Checking
					for(int i = 0; i < serv_total_connection; i++){
						printf("server: %s %d connect_cnt: %d\n",serv_info[i].serv_IP, serv_info[i].serv_PORT, serv_info[i].connect_cnt);
					}
				}
				if(user_algo == 2){
					for(int i = 0; i < serv_total_connection; i++){
						printf("server: %s %d resource: %d\n",serv_info[i].serv_IP, serv_info[i].serv_PORT, serv_info[i].resource_rate);
					}
				}
				
				int selected_serv_idx=lb_algorithm(user_algo, serv_info, &current_idx_RR);
				printf("\nselected server: %s %d connect_cnt: %d resource: %d \n\n",serv_info[selected_serv_idx].serv_IP, serv_info[selected_serv_idx].serv_PORT, serv_info[selected_serv_idx].connect_cnt, serv_info[selected_serv_idx].resource_rate);
				
				clnt_port = ntohs(tcp_header->source);
				uint16_t lb_PORT = (rand()%48127)+1024; //To avoid well-known port.
				makeTable(serv_info[selected_serv_idx], lb_PORT);
				for(int i = 0; i < table_connection_cnt; i++){ //Looking for whether it exists nor.
					if( (strcmp(table[i].clnt_IP, clnt_ip) == 0) && (table[i].clnt_PORT == ntohs(tcp_header->source)) ){
					       	current_table_idx = i;
					       	break;
					} 
				}	
				
			}	
			struct table *target_table = &table[current_table_idx];
			
			if(tcp_header->ack && target_table->syn_state > 0 && ((target_table->syn_seqNum+1) == ntohl(tcp_header->ack_seq))){ // For ACK about SYNACK
				target_table->syn_state++;
				if(target_table->syn_state >= 2){
					target_table->syn_state = 0;
					for(int i = 0; i < serv_total_connection; i++){
						if( (strcmp(target_table->serv_IP, serv_info[i].serv_IP) == 0) && (target_table->serv_PORT == serv_info[i].serv_PORT)){
							serv_info[i].connect_cnt++;
							//printf("connection up+++\n");
							break;
								}
					}
				}
			}else if(tcp_header->fin){
				target_table->fin_seqNum = ntohl(tcp_header->seq);
				target_table->fin_state++;
			}else if(target_table->fin_state > 0 && ((target_table->fin_seqNum+1) == ntohl(tcp_header->ack_seq))){ //For ACK about FIN
				target_table->fin_state++;
				if(target_table->fin_state >= 4) 
					target_table->fin_state = 0;
					for(int i = 0; i < serv_total_connection; i++){
						if( (strcmp(target_table->serv_IP, serv_info[i].serv_IP) == 0) && (target_table->serv_PORT == serv_info[i].serv_PORT)){
							serv_info[i].connect_cnt--;
							break;
								}
					}
				
			

			}
				
			if(replaceIP(ip_header, LB_IP, target_table->serv_IP)){
				fprintf(stderr,"ERROR: inet_pton\n");
				exit(1);	
			}		
			tcp_header->check = 0;
			
			replacePort(tcp_header,target_table->lb_PORT, target_table->serv_PORT); 

			makePacket(buffer, ip_header, tcp_header);	
			
			struct sockaddr_in daddr;	
			if(dst_setting(&daddr, target_table->serv_IP, target_table->serv_PORT)){
				fprintf(stderr,"ERROR: dst_setting\n");
				exit(1);
			}
			sendto(lb_clnt_sock, buffer, packet_len, 0, (struct sockaddr*)&daddr, sizeof(struct sockaddr));
		
		}else if(ep_events[i].data.fd == lb_clnt_sock){//Packet From Server
			packet_len = recvfrom(lb_clnt_sock, buffer, BUF_SIZE, 0, NULL, NULL);//Only Read from LB port openned for the clients	
			memcpy(&temp_port, buffer+22, sizeof(temp_port));
			
			temp_port = ntohs(temp_port);
			uint8_t connected_port = 0;
			int current_table_idx=0;
			for(int i = 0; i < table_connection_cnt; i++){
				if(table[i].lb_PORT == temp_port){
					connected_port = 1;
					current_table_idx = i;
					break;	
				}
			}
			if(connected_port != 1) continue;
    			struct iphdr *ip_header = (struct iphdr *)buffer;
			struct tcphdr *tcp_header = (struct tcphdr *)(buffer + (ip_header->ihl * 4));
			struct table *target_table = &table[current_table_idx];
			
			if(tcp_header->syn && tcp_header->ack){
				target_table->syn_seqNum = ntohl(tcp_header->seq);
				target_table->syn_state++;
			}else if(tcp_header->fin){
				target_table->fin_seqNum = ntohl(tcp_header->seq);
				target_table->fin_state++;
			}else if(target_table->fin_state > 0 && ((target_table->fin_seqNum + 1) == ntohl(tcp_header->ack_seq))){ //For ACK about FIN
				target_table->fin_state++;
				if(target_table->fin_state >= 4) 
					
					target_table->fin_state = 0;
					for(int i = 0; i < serv_total_connection; i++){
						if( (strcmp(target_table->serv_IP, serv_info[i].serv_IP) == 0) && (target_table->serv_PORT == serv_info[i].serv_PORT)){
							serv_info[i].connect_cnt--;
							break;
								}
					}
				
			

			}

			if(replaceIP(ip_header, LB_IP, target_table->clnt_IP)){
				exit(1);
			}
			ip_header->check = checksum((uint8_t*)ip_header, sizeof(struct iphdr));
			tcp_header->check = 0;
			replacePort(tcp_header,LB_PORT_FOR_CLIENT, target_table->clnt_PORT); 
			
			makePacket(buffer,ip_header, tcp_header);
			
			struct sockaddr_in daddr;	
			if(dst_setting(&daddr, target_table->clnt_IP, target_table->clnt_PORT)){
				fprintf(stderr,"ERROR: dst_setting\n");
				exit(1);
			}
			sendto(lb_serv_sock, buffer, packet_len, 0, (struct sockaddr*)&daddr, sizeof(struct sockaddr));
		}else{ //Among epoll socket, not lb_clnt_sock and lb_serv_sock. They are server socket for Heart-Beat/Resource-base through TCP connection
			
			packet_len = recv(ep_events[i].data.fd, buffer, BUF_SIZE, 0); 
			for(int i = 0; i < serv_total_connection; i++){
				if(serv_info[i].tcp_sock == ep_events[i].data.fd){
					serv_info[i].resource_rate = (uint16_t)atoi(buffer);
					break;
				}
			}
		}
	}
    }
    return 0;
}

