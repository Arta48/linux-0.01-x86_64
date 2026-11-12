#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#define S(x) do{ write(1,x"\n",sizeof(x)); }while(0)
int main(void){
  S("socketpair");
  int sv[2]; if(socketpair(AF_UNIX,SOCK_STREAM,0,sv)<0){perror("sp");return 1;}
  write(sv[0],"ab",2); char b[16]; int n=read(sv[1],b,16); printf("sp read %d\n",n);
  S("listen");
  int ls=socket(AF_INET,SOCK_STREAM,0); struct sockaddr_in a={0}; a.sin_family=AF_INET; a.sin_port=htons(7777); a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
  if(bind(ls,(void*)&a,sizeof a)<0){perror("bind");return 1;} if(listen(ls,4)<0){perror("listen");return 1;}
  S("connect");
  int c=socket(AF_INET,SOCK_STREAM,0); fcntl(c,F_SETFL,O_NONBLOCK);
  int r=connect(c,(void*)&a,sizeof a); printf("connect %d errno %d\n",r,errno);
  struct pollfd p={c,POLLOUT,0}; r=poll(&p,1,3000); printf("poll %d rev %x\n",r,p.revents);
  int s=accept(ls,NULL,NULL); printf("accept %d\n",s);
  fcntl(c,F_SETFL,0);
  write(c,"hello",5); n=read(s,b,16); printf("srv got %d\n",n);
  write(s,"world!",6); n=read(c,b,16); printf("cli got %d\n",n);
  char *big=malloc(500000); memset(big,'x',500000);
  if(!fork()){ close(c); close(ls); long t=0; while((n=read(s,big,500000))>0) t+=n; printf("child read %ld\n",t); _exit(0);} 
  close(s); long w=0; while(w<400000){ n=write(c,big,100000); if(n<=0)break; w+=n;} close(c); printf("wrote %ld\n",w); wait(0);
  S("udp");
  int u1=socket(AF_INET,SOCK_DGRAM,0), u2=socket(AF_INET,SOCK_DGRAM,0);
  a.sin_port=htons(9999); bind(u1,(void*)&a,sizeof a);
  sendto(u2,"dgram",5,0,(void*)&a,sizeof a); n=recv(u1,b,16,0); printf("udp got %d\n",n);
  S("done"); return 0;
}
