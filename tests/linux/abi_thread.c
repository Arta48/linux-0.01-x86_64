#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <fcntl.h>
static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
static int counter;
static void *w(void *a){ for(int i=0;i<1000;i++){ pthread_mutex_lock(&m); counter++; pthread_mutex_unlock(&m);} return (void*)(long)((long)a+1); }
int main(){
  pthread_t t[3]; void *r; long sum=0;
  for(long i=0;i<3;i++) pthread_create(&t[i],0,w,(void*)i);
  for(int i=0;i<3;i++){ pthread_join(t[i],&r); sum+=(long)r; }
  printf("threads: counter=%d sum=%ld\n",counter,sum);
  struct statfs sf; printf("statfs=%d\n", statfs("/",&sf));
  struct stat st; printf("stat=%d\n", stat("/etc/motd",&st));
  int in=open("/etc/motd",O_RDONLY), out=open("/tmp/cp.txt",O_WRONLY|O_CREAT|O_TRUNC,0644);
  off_t off=0; printf("sendfile=%ld\n",(long)sendfile(out,in,&off,100000));
  return 0; }
