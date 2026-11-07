#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <string.h>
static void h(int s){ write(1,"[sig]\n",6); }
int main(){
  write(1,"A\n",2);
  signal(SIGUSR1,h); int r=kill(getpid(),SIGUSR1);
  write(1,"B\n",2);
  int pid=fork();
  if(!pid){ write(1,"child writes\n",13); _exit(5);}
  write(1,"parent after fork\n",18);
  int st; int w=waitpid(pid,&st,0);
  char m[40]; m[0]='0'+(w>0); m[1]=' '; m[2]='0'+((st>>8)&7); m[3]='\n'; write(1,m,4);
  int pp[2]; pipe(pp);
  if(!fork()){ close(pp[0]); int n=write(pp[1],"piped\n",6); _exit(n); }
  close(pp[1]); char c[16]; int n=read(pp[0],c,15); write(1,"read:",5); if(n>0) write(1,c,n); else write(1,"none\n",5);
  wait(0);
  return 0;
}
