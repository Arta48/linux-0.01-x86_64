#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>
#include <dirent.h>
static void h(int s){ write(1,"[sig]\n",6); }
int main(int argc,char**argv){
  printf("hello from musl, argc=%d argv0=%s\n",argc,argv[0]);
  char *p=malloc(100000); memset(p,1,100000); free(p);
  void *q=malloc(5<<20); memset(q,2,5<<20); printf("big ok\n");
  int fd=open("/tmp/x.txt",O_CREAT|O_WRONLY|O_TRUNC,0644); write(fd,"abc\n",4); close(fd);
  char b[16]={0}; fd=open("/tmp/x.txt",O_RDONLY); read(fd,b,15); close(fd); printf("file: %s",b);
  DIR*d=opendir("/bin"); int n=0; struct dirent*e; while((e=readdir(d))) n++; closedir(d); printf("bin entries %d\n",n);
  signal(SIGUSR1,h); kill(getpid(),SIGUSR1);
  int pid=fork(); if(!pid){ printf("child\n"); _exit(7);} int st; waitpid(pid,&st,0); printf("child status %d\n",WEXITSTATUS(st));
  int pp[2]; pipe(pp); if(!fork()){ close(pp[0]); write(pp[1],"piped\n",6); _exit(0);} close(pp[1]); char c[16]={0}; read(pp[0],c,15); printf("%s",c); wait(0);
  printf("isatty %d\n", isatty(0));
  char line[64]; printf("type: "); fflush(stdout); if(fgets(line,64,stdin)) printf("got %s",line);
  return 3;
}
