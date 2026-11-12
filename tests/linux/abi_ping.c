#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <poll.h>
#include <arpa/inet.h>
#include <netinet/ip_icmp.h>
#include <sys/socket.h>
static unsigned short ck(void *b,int n){unsigned long s=0;unsigned short*p=b;for(;n>1;n-=2)s+=*p++;if(n)s+=*(unsigned char*)p;while(s>>16)s=(s&0xffff)+(s>>16);return ~s;}
static void one(int type,const char*ip){
  int s=socket(AF_INET,type,IPPROTO_ICMP); if(s<0){perror("socket");return;}
  struct sockaddr_in a={0}; a.sin_family=AF_INET; inet_pton(AF_INET,ip,&a.sin_addr);
  struct icmphdr h; memset(&h,0,sizeof h); h.type=ICMP_ECHO; h.un.echo.id=htons(0x4242); h.un.echo.sequence=htons(1); h.checksum=ck(&h,sizeof h);
  if(sendto(s,&h,sizeof h,0,(void*)&a,sizeof a)<0){perror("sendto");return;}
  struct pollfd p={s,POLLIN,0}; int r=poll(&p,1,2000); char b[200];
  int n=r>0?recv(s,b,sizeof b,0):-1; printf("type %d to %s: poll %d recv %d\n",type,ip,r,n); close(s);
}
int main(int c,char**v){ const char*ip=c>1?v[1]:"10.0.2.2"; one(SOCK_RAW,ip); one(SOCK_DGRAM,ip); one(SOCK_RAW,"127.0.0.1"); return 0; }
