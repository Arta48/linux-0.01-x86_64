#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <poll.h>
#include <sys/eventfd.h>
int main(void){
  int e=eventfd(0,EFD_NONBLOCK|EFD_CLOEXEC); if(e<0){perror("eventfd");return 1;}
  struct pollfd p={e,POLLIN,0}; printf("poll empty %d\n",poll(&p,1,0));
  uint64_t v=3; write(e,&v,8); v=4; write(e,&v,8);
  printf("poll full %d\n",poll(&p,1,0));
  v=0; int n=read(e,&v,8); printf("read %d value %lu\n",n,(unsigned long)v);
  n=read(e,&v,8); printf("again %d\n",n);
  return 0; }
