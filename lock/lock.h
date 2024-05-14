#pragma once
#include<exception>
#include<pthread.h>
#include<semaphore.h>
#include<time.h>
using namespace std;
class sem{
public:
    //默认构造函数
    sem(){
        //init_sem()函数：
        //参数一：sem_t类型的指针
        //参数二：参数二为0 则表示在进程的线程间分享、为非0 则表示在进程间分享
        //参数三：设置信号量的初始值
        //成功返回0，失败返回-1
        if(sem_init(&m_sem,0,0)!=0){
            throw exception();
        } 
    }
    //含参构造函数
    sem(int num){
        if(sem_init(&m_sem,0,num)!=0){
            throw exception();
        }
    }
    ~sem(){
        sem_destroy(&m_sem);
    }

    //wait 减操作
    bool wait(){
        return sem_wait(&m_sem)==0;
    }
    //post 加操作 并发送信号
    bool post(){
        return sem_post(&m_sem)==0;
    }


private:
    sem_t m_sem;

};

class locker{
public:
    //locker构造函数
    locker(){
        if(pthread_mutex_init(&m_mutex,NULL)!=0){
            throw exception();
        }
    }
    //locker析构函数
    ~locker(){
        pthread_mutex_destroy(&m_mutex);
    }
    //枷锁
    bool lock(){
        return pthread_mutex_lock(&m_mutex)==0;
    }
    //解锁
    bool unlock(){
        return pthread_mutex_unlock(&m_mutex)==0;
    }
    //获取pthread_mutex_t类型的指针
    pthread_mutex_t * get(){
        return &m_mutex;
    }


private:
    pthread_mutex_t m_mutex;
};

//cond条件类
class cond
{
private:
    pthread_cond_t m_cond;
public:
    //构造函数
    cond(){
        if(pthread_cond_init(&m_cond,NULL)!=0){
            throw exception();
        }
    }
    //析构函数
    ~cond(){
        pthread_cond_destroy(&m_cond);
    }
    //wait 减函数
    bool wait(pthread_mutex_t*m_mutex){
        int ret = 0;
        ret = pthread_cond_wait(&m_cond, m_mutex);
        return ret == 0;
        //return pthread_cond_wait(&m_cond,m_mutex)==0;
    }
    //timewait函数
    bool timewait(pthread_mutex_t*m_mutex,struct timespec t){
        int ret = 0;
        ret = pthread_cond_timedwait(&m_cond, m_mutex, &t);
        return ret == 0;
        //return pthread_cond_timedwait(&m_cond,m_mutex,&t)==0;
    }
    //唤醒单个等待的线程
    bool signal(){
        return pthread_cond_signal(&m_cond)==0;
    }
    //唤醒所有等待的线程
    bool boardcast(){
        return pthread_cond_broadcast(&m_cond)==0;
    }
};