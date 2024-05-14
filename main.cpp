#include<iostream>
#include<sys/types.h>
#include <sys/socket.h>
#include<netinet/in.h>
#include <arpa/inet.h>
#include<string.h>
#include<string>
#include <stdio.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/epoll.h>
#include <cstdlib>
#include <fcntl.h>
#include <errno.h>

#include"./lock/lock.h"
#include"./ThreadPool/threadpool.hpp"
#include"./timer/lst_timer.hpp"
#include"./http/http_conn.h"
#include"./log/log.h"
#include"./CGImysql/sql_conn_pool.h"


using namespace std;
const int MAX_EVENTS=10000;//最大的世界数量
const int MAX_FD=65536;//最大的文件描述符数量
const int TIMESLOT=5;//等待时间

//#define SYNLOG//同步写日志
#define ASYNLOG//异步写日志

//#define listenfdET//边缘触发非阻塞
#define listenfdLT//水平触发阻塞

//这三个函数在http_conn.cpp中定义，改变链接属性
extern int addfd(int epollfd, int fd, bool one_shot);
extern int remove(int epollfd, int fd);
extern int setnonblocking(int fd);

//设置定时器相关函数
static int pipefd[2];
static sort_timer_lst timer_lst;
static int epollfd=0;

//信号处理函数
void sig_handler(int sig){
    //保证函数的可重入性，保留原来的errno
    int save_errno=errno;//将错误信息保存
    int msg=sig;
    send(pipefd[1],(char*)&msg,1,0);
    errno=save_errno;
}

//设置信号函数     handler作为函数指针指向一个 参数为int返回值类型为void的函数
void addsig(int sig,void (handler)(int),bool restart=true){
    struct sigaction sa;//sigaction可以用来查询或设置信号处理方式
    memset(&sa,'\0',sizeof(sa));
    sa.sa_handler=handler;
    if(restart){
        sa.sa_flags|=SA_RESTART;
    }
    sigfillset(&sa.sa_mask);//将sa.sa_mask指定的信号集中的信号都设置为阻塞
    assert(sigaction(sig,&sa,NULL)!=-1);//检验是否发生错误
}

//定时处理任务，重新定时以不断出发SIGALRM信号
void timer_handler(){
    timer_lst.tick();
    alarm(TIMESLOT);//等待TIMESLOT秒后的信号
}

//定时器回调函数，删除非活动连接在socket上的注册事件，并关闭
void cb_func(client_data*user_data){
    epoll_ctl(epollfd,EPOLL_CTL_DEL,user_data->sockfd,0);
    assert(user_data);
    close(user_data->sockfd);
    http_conn::m_user_count--;
    LOG_INFO("close fd &d",user_data->sockfd);
    Log::get_instance()->flush();
}

void show_error(int connfd,const char*info){
    cout<<info;
    send(connfd,info,strlen(info),0);
    close(connfd);
}


int main(int argc,char *argv[]){

#ifdef ASYNLOG              //文件名、缓冲区大小、最大行数、最大队列大小
    Log::get_instance()->init("ServerLog",2000,800000,8);//异步日志模型
#endif

#ifdef SYNLOG
    Log::get_instance()->init("ServerLog",2000,800000,0);//同步日志模型
#endif

    if(argc<=1){//判断传入的参数是否足够（需要地址和端口）
        cout<<"usage:"<<basename(argv[0])<<"ip_address port_number"<<endl;
        return 1;
    }

    //传入端口号
    int port=stoi(argv[1]);

    addsig(SIGPIPE,SIG_IGN);//使SIGPIPE信号失效   第三个参数默认为true

    //创建数据库连接池
    sql_connection_pool *connPool=sql_connection_pool::getInstance();
    connPool->init("localhost","root","admin","yourdb",3306,8);

    //创建线程池
    threadpool<http_conn>*pool=NULL;
    try{
        pool=new threadpool<http_conn>(connPool);
    }
    catch(...){//...表示捕获所有的异常
        return 1;
    }

    //创建http连接实例users
    http_conn*users=new http_conn[MAX_FD];
    assert(users);

    //初始化数据库读取表
    users->initmysql_result(connPool);

    //创建一个监听描述符
    int listenfd=socket(AF_INET,SOCK_STREAM,0);
    if(listenfd<0){
    perror("listenfd error:");
    }
    //bind需要的的套接字的地址的sockaddr结构
    struct sockaddr_in address;
    memset(&address,0,sizeof((address)));
    address.sin_family=AF_INET;
    address.sin_port=htons(port);//主机转换为网络字节序短整形
    address.sin_addr.s_addr=htonl(INADDR_ANY);//主机转换成网络字节序长整型
    //地址复用
    int flag=1;
    setsockopt(listenfd,SOL_SOCKET,SO_REUSEADDR,&flag,sizeof(flag));
    //连接客户端和服务端
    int ret=bind(listenfd,(struct sockaddr*)&address,sizeof(address));
    if(ret<0){
        perror("bind error:");
    }
    ret=listen(listenfd,5);//多少个最多128
    if(ret<0){
        perror("listen error:");
    }
    // //接受客户端请求形成通信cfd连接文件描述符
    // int cfd=accept(listenfd,NULL,NULL);
    // cout<<"listenfd: "<<listenfd<<endl;
    // cout<<"cfd: "<<cfd<<endl;
    // //循环读取和发送数据
    // int n;
    // char buf[1024];
    // int i=0;
    // while(1){
    //     //读取客户端传来的数据
    //     memset(&buf,0,sizeof(buf));
    //     int n=read(cfd,buf,sizeof(buf));
    //     //转换一下
    //     for(i=0;i<n;++i){
    //         buf[i]=toupper(buf[i]);
    //     }
    //     //发送数据给客户端
    //     write(cfd,buf,n);
    // }
    
    //用epoll来监管客户端发来的消息
    //创建epoll实例
    epollfd=epoll_create(5);
    if(epollfd==-1){
        perror("epoll error:");
        exit(EXIT_FAILURE);
    }
    //创建事件表
    epoll_event ev[MAX_EVENTS];
    // //将listenfd添加到epoll_event事件中监听读事件
    // ev->events=EPOLLIN;
    // ev->data.fd=listenfd;
    // if(epoll_ctl(epollfd,EPOLL_CTL_ADD,listenfd,(epoll_event*)&ev)){
    //     perror("epoll_ctl listenfd error:");
    //     exit(EXIT_FAILURE);
    // }
    assert(epollfd!=-1);
    addfd(epollfd,listenfd,false);
    http_conn::m_epollfd=epollfd;

    //创建管道
    ret=socketpair(PF_UNIX,SOCK_STREAM,0,pipefd);//tcp模式的
    assert(ret!=-1);
    setnonblocking(pipefd[1]);
    addfd(epollfd,pipefd[0],false);

    addsig(SIGALRM,sig_handler,false);//设置信号函数
    addsig(SIGTERM,sig_handler,false);
    bool stop_server=false;//停止服务标志

    client_data*users_timer=new client_data[MAX_FD]; //创建一个客户数据结构体

    bool timeout=false;
    alarm(TIMESLOT);

    //处理事件
    while(!stop_server){//当没有停止服务的时候
        //等待epoll事件
        int nfds=epoll_wait(epollfd,ev,MAX_EVENTS,-1);//-1表示一直等直到有
        if(nfds<0 && errno!=EINTR){
            LOG_ERROR("epoll_wait error:");
            break;
        }
        for(int i=0;i<nfds;++i){
            int socketfd=ev[i].data.fd;//一个一个全都拿出来
            int n=0;
            char buf[1024];
            if(socketfd==listenfd){//如果是监听描述符
                //处理新的连接
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof(client_address);
#ifdef listenfdLT
                int cfd=accept(listenfd,(struct sockaddr*)&client_address,&client_addrlength);
                //测试
                // cout<<"listenfd: "<<listenfd<<endl;
                // cout<<"epollfd: "<<epollfd<<endl;
                // cout<<"cfd: "<<cfd<<endl;

                if(cfd<0){
                    LOG_ERROR("%s:error is:%d","accept error",errno);
                    continue;
                }
                //如果用户数量大于允许的最大的文件描述符数量
                if(http_conn::m_user_count>=MAX_FD){
                    show_error(cfd,"Internal server busy");
                    LOG_ERROR("%s","Internal server busy");
                    continue;
                }
                users[cfd].init(cfd,client_address);
                //将新文件描述符添加到epoll实例中去
                // ev->events=EPOLLIN;
                // ev->data.fd=cfd;
                // if(epoll_ctl(epollfd,EPOLL_CTL_ADD,cfd,(epoll_event*)&ev)==-1){//将添加步骤和判断是否错误结合在一起
                //     perror("epoll_ctl cfd error");
                //     close(cfd);
                // }
                // setnonblocking(cfd);
                //addfd(epollfd,cfd,false);
                
                //初始化client_data数据
                //创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中
                users_timer[cfd].address=client_address;//客户端地址存入users
                users_timer[cfd].sockfd = cfd;//通讯描述符存入users
                util_timer *timer = new util_timer;//创建util_timer类的实例
                timer->user_data = &users_timer[cfd];//user_data写入
                timer->cb_func = cb_func;//回调函数写入
                time_t cur = time(NULL);//新建一个time_t类型的 实例
                timer->expire = cur + 3 * TIMESLOT;//增加了三倍的等待时间
                users_timer[cfd].timer = timer;
                timer_lst.add_timer(timer);
#endif

#ifdef listenfdET  //就只比LT多了一个while(1)

                while (1)
                {
                    int cfd = accept(listenfd, (struct sockaddr *)&client_address, &client_addrlength);
                    if (cfd < 0)
                    {
                        LOG_ERROR("%s:errno is:%d", "accept error", errno);
                        break;
                    }
                    if (http_conn::m_user_count >= MAX_FD)
                    {
                        show_error(cfd, "Internal server busy");
                        LOG_ERROR("%s", "Internal server busy");
                        break;
                    }
                    users[cfd].init(cfd, client_address);

                    //初始化client_data数据
                    //创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中
                    users_timer[cfd].address = client_address;
                    users_timer[cfd].sockfd = cfd;
                    util_timer *timer = new util_timer;
                    timer->user_data = &users_timer[cfd];
                    timer->cb_func = cb_func;
                    time_t cur = time(NULL);
                    timer->expire = cur + 3 * TIMESLOT;
                    users_timer[cfd].timer = timer;
                    timer_lst.add_timer(timer);
                }
                continue;
#endif

            }

            //定时器相关
            else if(ev[i].events&(EPOLLRDHUP|EPOLLHUP|EPOLLERR)){
                //服务器端关闭连接，移除对应的定时器
                util_timer*timer=users_timer[socketfd].timer;
                timer->cb_func(&users_timer[socketfd]);//回调函数执行关闭移除操作

                //删除timer
                if(timer){
                    timer_lst.del_timer(timer);
                }
            }

            //处理信号用到了pipe管道通信 看发过来什么信号 执行相应变化
             //pipefd[0]表示读端
            else if ((socketfd == pipefd[0]) && (ev[i].events & EPOLLIN))
            {
                int sig;
                char signals[1024];
                ret = recv(pipefd[0], signals, sizeof(signals), 0);
                if (ret == -1)
                {
                    continue;
                }
                else if (ret == 0)
                {
                    continue;
                }
                else
                {
                    for (int i = 0; i < ret; ++i)
                    {
                        switch (signals[i])
                        {
                        case SIGALRM://如果是ALRM信号则将timeout设置为true
                        {
                            timeout = true;
                            break;
                        }
                        case SIGTERM://如果是SIGTERM信号则将stop_server设置为true
                        {
                            stop_server = true;
                        }
                        }
                    }
                }
            }

            //处理客户连接上接收到的数据
            else if (ev[i].events & EPOLLIN)
            {
                util_timer *timer = users_timer[socketfd].timer;
                if (users[socketfd].read_once())
                {
                    LOG_INFO("deal with the client(%s)", inet_ntoa(users[socketfd].get_address()->sin_addr));
                    Log::get_instance()->flush();
                    //若监测到读事件，将该事件放入请求队列
                    pool->append(users + socketfd);//pool为线程池

                    //若有数据传输，则将定时器往后延迟3个单位
                    //并对新的定时器在链表上的位置进行调整
                    if (timer)
                    {
                        time_t cur = time(NULL);
                        timer->expire = cur + 3 * TIMESLOT;
                        LOG_INFO("%s", "adjust timer once");
                        Log::get_instance()->flush();
                        timer_lst.adjust_timer(timer);
                    }
                }
                else//否则关闭删除timer  （util_timer类型的）
                {
                    timer->cb_func(&users_timer[socketfd]);
                    if (timer)
                    {
                        timer_lst.del_timer(timer);
                    }
                }
            }
            else if (ev[i].events & EPOLLOUT)
            {
                util_timer *timer = users_timer[socketfd].timer;
                if (users[socketfd].write())
                {
                    LOG_INFO("send data to the client(%s)", inet_ntoa(users[socketfd].get_address()->sin_addr));
                    Log::get_instance()->flush();

                    //若有数据传输，则将定时器往后延迟3个单位
                    //并对新的定时器在链表上的位置进行调整
                    if (timer)
                    {
                        time_t cur = time(NULL);
                        timer->expire = cur + 3 * TIMESLOT;
                        LOG_INFO("%s", "adjust timer once");
                        Log::get_instance()->flush();
                        timer_lst.adjust_timer(timer);
                    }
                }
                else
                {
                    timer->cb_func(&users_timer[socketfd]);
                    if (timer)
                    {
                        timer_lst.del_timer(timer);
                    }
                }
            }
        }
        if (timeout)
        {
            timer_handler();
            timeout = false;
        }
    }

    close(listenfd);
    close(epollfd);
    close(pipefd[0]);
    close(pipefd[1]);
    delete[]users;
    delete[]users_timer;
    delete pool;

    return 0;
}

