#pragma once
#include<list>
#include<stdio.h>
#include<exception>
#include<pthread.h>
#include"../lock/lock.h"
#include"../CGImysql/sql_conn_pool.h"
using namespace std;

template<class T>
class threadpool
{
private:
    //工作运行成员函数
    static void *worker(void*arg);
    //启动运行成员函数
    void run();
    //成员变量
    int m_thread_number;//线程池中线程数量
    pthread_t *m_threads;//线程池数组
    int m_max_requests;//请求队列中最大数量
    list<T*>m_workqueue;//请求队列
    locker m_queuelocker;//保护请求队列的互斥锁
    sem m_queuestat;//队列状态，是否需要处理任务
    bool m_stop;//结束标志符
    sql_connection_pool *m_connPool;//数据库指针


public:
    threadpool(sql_connection_pool*connPool,int thread_number=8,int max_request=10000 );
    ~threadpool();
    bool append(T*request);
};

//构造函数类内声明，类外实现
template<class T>
threadpool<T>::threadpool(sql_connection_pool*connpool,int thread_number, int max_request):m_thread_number(thread_number),m_max_requests(max_request),m_stop(false),m_threads(NULL),m_connPool(connpool)
{
    if(thread_number<=0||max_request<=0){
        throw exception();
    }
    m_threads=new pthread_t[m_thread_number];
    if(!m_threads){
        throw exception();
    }
    for(int i=0;i<thread_number;++i){
        //创建线程顺便判断是否出错
        if(pthread_create(m_threads+i,NULL,worker,this)!=0){
            delete[]m_threads;
            throw exception();
        }
        //将线程分离，之后就不用join来结束进程，并且判断是否出错
        if(pthread_detach(m_threads[i])){//因为成功返回0，所以失败则执行
            delete[]m_threads;
            throw exception();
        }
    }
}

//类内声明类外实现的析构函数
template<class T>
threadpool<T>::~threadpool()
{
    delete[]m_threads;
    m_stop=true;
}

//类内声明类外实现的append函数
template<class T>
bool threadpool<T>::append(T*request){
    m_queuelocker.lock();
    if(m_workqueue.size()>m_max_requests){
        m_queuelocker.unlock();
        return false;
    }
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post();//sem类中的函数
    return true;
}

//类内声明的worker函数类外实现
//回调函数，当线程被运行时执行此函数
template<class T>
void * threadpool<T>::worker(void*arg){
    threadpool*pool=(threadpool*)arg;//将worker函数中的参数指向调用worker函数的threadpool类实例
    pool->run();//回调函数：回过头来调用调用worker函数的类里面的run函数
    return pool;
}

//类内声明类外实现的run函数
template<class T>
void threadpool<T>::run(){
    while(!m_stop){
        m_queuestat.wait();
        m_queuelocker.lock();
        if(m_workqueue.empty()){
            m_queuelocker.unlock();
            continue;
        }
        T*request=m_workqueue.front();
        m_workqueue.pop_front();
        m_queuelocker.unlock();
        if(!request){
            continue;
        }
        //request是什么？
        connectionRAII mysqlcon(&request->mysql,m_connPool);

        request->process();//运行请求过程
    }
}