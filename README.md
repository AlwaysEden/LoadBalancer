# LoadBalancer

## How to start
```
1. gcc loadbalancer.c -o loadbalancer
2. ./loadbalancer (RR/AL/RB)

The argument means the algorithm to operate loadbalancer.
RR = Round Robin, Loadbalancer choose the server in order. 
AL = At least, Loadbalancer choose the server with least connection.
RB = Resource Based, Loadbalancer choose the server with minimum cpu and memory consumption.
```
## About
Environmnet: Raspberrypi/Raspbian OS   
Language: C   
Test Tool: Flask(For Web service)   

## Interesting technical skill
- Epoll
- RawSocket
- Loadbalancer(Type: Direct Server Return)
