/* File system calls: open/read/write/lseek/stat/dirs/rename/pipes/dup, 64-bit offsets. */
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

int main(void)
{
    char dir[] = "/tmp/a32test.XXXXXX";
    if (!mkdtemp(dir)) return 1;
    char path[256];
    snprintf(path, sizeof path, "%s/f1", dir);
    int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0640);
    const char *msg = "hello file\n";
    write(fd, msg, strlen(msg));
    struct iovec iov[2] = {{(void *)"ab", 2}, {(void *)"cdef", 4}};
    writev(fd, iov, 2);
    off_t end = lseek(fd, 0, SEEK_END);
    off64_t big = lseek64(fd, 5LL << 30, SEEK_SET);
    write(fd, "Z", 1);
    struct stat st;
    fstat(fd, &st);
    printf("end %ld big %lld size %lld mode %o blocks>0 %d\n", (long)end, (long long)big, (long long)st.st_size,
           st.st_mode & 0777, st.st_blocks > 0);
    char buf[64] = {0};
    pread(fd, buf, 17, 0);
    printf("pread [%s]\n", buf);
    close(fd);
    struct stat st2;
    stat(path, &st2);
    printf("stat ino match %d nlink %d\n", st.st_ino == st2.st_ino, (int)st2.st_nlink);
    char p2[256];
    snprintf(p2, sizeof p2, "%s/f2", dir);
    rename(path, p2);
    printf("access old %d new %d\n", access(path, F_OK), access(p2, R_OK));
    snprintf(path, sizeof path, "%s/sub", dir);
    mkdir(path, 0755);
    symlink("f2", strcat(strcpy(buf, dir), "/link"));
    char lbuf[64] = {0};
    readlink(buf, lbuf, sizeof lbuf);
    printf("readlink %s\n", lbuf);
    DIR *d = opendir(dir);
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d))) n += e->d_name[0] != '.';
    closedir(d);
    printf("entries %d\n", n);
    int p[2];
    pipe(p);
    write(p[1], "pipe!", 5);
    int dupfd = dup(p[0]);
    memset(buf, 0, sizeof buf);
    read(dupfd, buf, 5);
    printf("pipe [%s] cloexec %d\n", buf, fcntl(p[0], F_GETFD));
    unlink(buf);
    unlink(p2);
    rmdir(path);
    unlink(strcat(strcpy(buf, dir), "/link"));
    printf("rmdir %d\n", rmdir(dir));
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    printf("clock ok %d\n", ts.tv_sec >= 0);
    char cwd[256];
    printf("getcwd ok %d\n", getcwd(cwd, sizeof cwd) != NULL);
    printf("exe ok %d\n", readlink("/proc/self/exe", lbuf, sizeof lbuf) > 0);
    return 0;
}
