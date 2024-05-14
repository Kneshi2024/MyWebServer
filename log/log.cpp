#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <stdarg.h>
#include "log.h"
#include <pthread.h>

using namespace std;

//构造函数初始化
Log::Log()
{
    m_count = 0;
    m_is_async = false;//是否异步
}

//析构函数
Log::~Log()
{
    //将文件的描述符关闭
    if (m_fp != NULL)
    {
        fclose(m_fp);
    }
}

//异步需要设置指针的长度，同步不需要设置
bool Log::init(const char*file_name,int log_buf_size,int split_lines,int max_queue_size){
    //如果设置了max_queue_size,则设置为异步
    if(max_queue_size>=1){
        m_is_async=true;
        m_log_queue=new block_queue<string>(max_queue_size);
        pthread_t tid;
        //flush_log_thread为回调函数，这里表示创建线程异步写日志
        pthread_create(&tid,NULL,flush_log_thread,NULL);
    }

    m_log_buf_size = log_buf_size;
    m_buf = new char[m_log_buf_size];
    memset(m_buf, '\0', m_log_buf_size);
    m_split_lines = split_lines;

    time_t t = time(NULL);//创建一个time_t类型的变量
    struct tm *sys_tm = localtime(&t);//系统时间=当前时间
    struct tm my_tm = *sys_tm;//我的时间指针=系统时间指针

    const char *p = strrchr(file_name, '/');//从右往左找到第一个‘/’,返回下标
    char log_full_name[256] = {0};
    //表示没有/就没有目录文件
     if (p == NULL){//将...存入到log_buf_name中去
        snprintf(log_full_name, 255, "%d_%02d_%02d_%s", my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday, file_name);
    }
    else{
        strcpy(log_name, p + 1);//将/后面的字符串拷贝到log_name中去
        strncpy(dir_name, file_name, p - file_name + 1);//表示有 / 就有目录文件,将/前的拷贝到dir_name中去
        snprintf(log_full_name, 255, "%s%d_%02d_%02d_%s", dir_name, my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday, log_name);
    }

    m_today = my_tm.tm_mday;

    m_fp = fopen(log_full_name, "a");//‘a’表示追加模式
    if (m_fp == NULL){
        return false;
    }
    return true;
}

void Log::write_log(int level,const char*format,...){
    struct timeval now = {0, 0};// timeval 初始化为0
    gettimeofday(&now, NULL);//获取当前时间
    time_t t = now.tv_sec;//time_t 类型的 t=timeval类中的tv_sec(秒数)
    struct tm *sys_tm = localtime(&t);//系统时间=当前时间
    struct tm my_tm = *sys_tm;//我的时间指针=系统时间指针
    char s[16] = {0};//新建的s缓存区
    switch (level){
    case 0:
        strcpy(s, "[debug]:");//调试
        break;
    case 1:
        strcpy(s, "[info]:");//信息
        break;
    case 2:
        strcpy(s, "[warn]:");//警告
        break;
    case 3:
        strcpy(s, "[erro]:");//错误
        break;
    default:
        strcpy(s, "[info]:");//其他也用信息代替
        break;
    }

    //写入一个log，对m_count++, m_split_lines最大行数
    m_mutex.lock();//上锁
    m_count++;//日志行数++

    //如果今天和记录的日期不同或是行数是日志最大行数的整数倍
    if (m_today != my_tm.tm_mday || m_count % m_split_lines == 0){
        
        char new_log[256] = {0};//新建log文件
        fflush(m_fp);//刷新流的缓冲区，以确保缓冲区中的数据被写入到文件中
        fclose(m_fp);//却把旧的文件描述符已经被关闭
        char tail[16] = {0};//尾部
        //尾部=几年几月几日（2024年04月27日）
        snprintf(tail, 16, "%d_%02d_%02d_", my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday);
        //日期非今天则新建一个log文件
        if (m_today != my_tm.tm_mday)
        {
            snprintf(new_log, 255, "%s%s%s", dir_name, tail, log_name);//由目录+尾部+日志名组成
            m_today = my_tm.tm_mday;
            m_count = 0;
        }
         else{//如果是同一天的则新建一个log文件 且在最后加上 页码
            snprintf(new_log, 255, "%s%s%s.%lld", dir_name, tail, log_name, m_count / m_split_lines);
        }
        m_fp = fopen(new_log, "a");//打开新的文件描述符
    }
    m_mutex.unlock();

    va_list valst;//用于存储关于可变参数列表的信息
    va_start(valst, format);//用于初始化 va_list 变量 valst，使其指向第一个可变参数

    string log_str;
    m_mutex.lock();

    //写入的具体时间内容格式  （年、月、日、时、分、秒、微秒、状态标志）
    int n = snprintf(m_buf, 48, "%d-%02d-%02d %02d:%02d:%02d.%06ld %s ",
                     my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday,
                     my_tm.tm_hour, my_tm.tm_min, my_tm.tm_sec, now.tv_usec, s);

    int m = vsnprintf(m_buf + n, m_log_buf_size - 1, format, valst);//将valst放入m_buf第n个位置开始
    m_buf[n + m] = '\n';
    m_buf[n + m + 1] = '\0';
    log_str = m_buf;

    m_mutex.unlock();

    //如果是异步且日志队列非空
    if (m_is_async && !m_log_queue->full())//m_log_queue为阻塞队列
    {
        m_log_queue->push(log_str);
    }
    else
    {
        m_mutex.lock();
        fputs(log_str.c_str(), m_fp);//将参数一写入到参数二中去
        m_mutex.unlock();
    }

    va_end(valst);  
}

void Log::flush(void)
{
    m_mutex.lock();
    //强制刷新写入流缓冲区
    fflush(m_fp);
    m_mutex.unlock();
}