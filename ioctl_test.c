#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioc_secret.h>


int main( int argc, char *argv[ ], char *envp[ ] )
{
   int fd, res;
   char msg[100] = "hello world";
   int uid;

   fd = open("/dev/Secret", O_WRONLY); 
   printf("Opening... fd=%d\n",fd); 
   res = write(fd,msg,strlen(msg)); 
   printf("Writing... res=%d\n",res); /* try grant */

   if ( argc > 1 && 0 != (uid = atoi(argv[1]))) { 
      if ( res = ioctl(fd, SSGRANT, &uid) ) {
         perror("ioctl");
      } else {
         printf("res: %d\n",res);
      }

      printf("Trying to change owner to %d.. res=%d\n",uid, res);
   }

   res=close(fd);

   return 1;
}
