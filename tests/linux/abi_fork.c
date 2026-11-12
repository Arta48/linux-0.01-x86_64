#include <unistd.h>
#include <sys/wait.h>
int main(){
  int pid=fork();
  if(!pid){ int n=write(1,"child writes\n",13); _exit(n<0?-n:100+n); }
  int st; waitpid(pid,&st,0);
  char m[8]; int v=(st>>8)&255; m[0]='0'+v/100; m[1]='0'+(v/10)%10; m[2]='0'+v%10; m[3]='\n'; write(1,m,4);
  return 0;
}
