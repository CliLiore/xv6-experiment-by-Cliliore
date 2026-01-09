#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int main(int argc, char *argv[]) {
  //输入参数不为2时，打印错误信息并退出
  if(argc != 2) {
    //错误打印到标准错误输出
    fprintf(2, "错误:请使用sleep <time>\n");
    exit(1);
  }

  //将字符串参数转换为整数
  int time = atoi(argv[1]);
  
  //调用sleep
  sleep(time);
  
  //退出程序
  exit(0);
}