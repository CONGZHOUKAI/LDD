#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h> 
#include <string.h>

int main(int argc, char *argv[])
{
    int fd;
    char buf[100];
    u_int32_t cnt = 0;
    fd = open("/dev/sc", O_WRONLY);
    if (fd < 0)
    {   
        perror("test");
    }

    for (;;)
    {
        cnt++;
        sprintf(buf, "+++++++++++++++=%d\n", cnt);
        write(fd, buf, strlen(buf));
    }
    
    

    close(fd);
    return 0;
}