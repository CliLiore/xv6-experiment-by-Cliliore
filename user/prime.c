#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

//筛选质数函数
__attribute__((noreturn))void sieve(int read_fd) {
    int prime;//当前质数
    int num;//从管道读取的数字
    
    //从管道读端读取一个数字，它一定是质数
    if (read(read_fd, &prime, sizeof(prime)) == 0) {//读取完成
        close(read_fd);//关闭读端
        exit(0);
    }
    
    printf("prime %d\n", prime);//打印当前质数
    
    int pipe_fd[2];//创建新管道
    if (pipe(pipe_fd) != 0) {
        fprintf(2, "管道创建失败\n");
        exit(1);
    }
    
    int ccr = fork();//创建新子进程
    if (ccr < 0) {
        fprintf(2, "子进程创建失败\n");
        exit(1);
    }

    if (ccr == 0) {
        //子进程继续筛选下一个质数
        close(pipe_fd[1]);//关闭写端
        close(read_fd);//关闭父进程的管道
        sieve(pipe_fd[0]);//调用质数筛选函数，递归筛选
    } else {
        //父进程继续过滤数字
        close(pipe_fd[0]);//关闭读端
        
        //读取并过滤数字
        while (read(read_fd, &num, sizeof(num)) > 0) {
            //如果不能被当前质数整除，则传递给下一个进程
            if (num % prime != 0) {
                write(pipe_fd[1], &num, sizeof(num));
            }
        }
        
        close(read_fd);
        close(pipe_fd[1]);
        wait(0);//等待子进程完成
        exit(0);
    }
}

int main(int argc, char *argv[]) {
    int pipe_fd[2];//创建首个管道
    if (pipe(pipe_fd) < 0) {
        fprintf(2, "管道创建失败\n");
        exit(1);
    }
    
    int ccr = fork();//创建首个子进程
    if (ccr < 0) {
        fprintf(2, "子进程创建失败\n");
        exit(1);
    }
    
    if (ccr == 0) {
        //子进程开始筛选
        close(pipe_fd[1]);//关闭写端
        sieve(pipe_fd[0]);//开始筛选
    } else {
        //父进程生成数字2to280
        close(pipe_fd[0]);//关闭读端
        
        for(int i = 2; i <= 280; i++) {
            write(pipe_fd[1], &i, sizeof(i));//写入管道传递给子进程
        }
        
        close(pipe_fd[1]);
        wait(0);//等待子进程筛选完成
        exit(0);
    }
    
    return 0;
}