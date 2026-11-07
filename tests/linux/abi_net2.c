#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
int main(int argc,char**argv){
  struct sockaddr_in a={0}; a.sin_family=AF_INET; a.sin_port=htons(1); a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
  if(argc>1){ inet_pton(AF_INET,argv[1],&a.sin_addr); a.sin_port=htons(atoi(argv[2])); }
  int c=socket(AF_INET,SOCK_STREAM,0); fcntl(c,F_SETFL,O_NONBLOCK);
  int r=connect(c,(void*)&a,sizeof a); printf("connect %d errno %d\n",r,errno); fflush(stdout);
  struct pollfd p={c,POLLOUT|POLLIN,0}; r=poll(&p,1,3000); printf("poll %d rev %x\n",r,p.revents);
  int e=0; socklen_t l=4; getsockopt(c,SOL_SOCKET,SO_ERROR,&e,&l); printf("soerr %d\n",e);
  return 0; }
