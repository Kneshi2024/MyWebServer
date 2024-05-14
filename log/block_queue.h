#pragma once
#include<iostream>
#include<stdlib.h>
#include<pthread.h>
#include<sys/time.h>
#include"../lock/lock.h"

using namespace std;

//阻塞队列
template<class T>
class block_queue
{
private:
    locker m_mutex;//互斥锁
    cond m_cond;//条件变量

    T*m_array;
    int m_size;
    int m_max_size;
    int m_front;
    int m_back;

public:
    //构造函数初始化
    block_queue(int max_size=1000){
        if(max_size<=0){
            exit(-1);
        }

        m_max_size=max_size;
        m_array= new T[max_size];
        m_size=0;
        m_front=-1;
        m_back=-1;
    }

    //清空成员变量
    void clear(){
        m_mutex.lock();
        m_size=0;
        m_front=-1;
        m_back=-1;
        m_mutex.unlock();
    }
    //析构函数
    ~block_queue(){
        m_mutex.lock();
        if(m_array!=NULL){
            delete[]m_array;
        }
        m_mutex.unlock();
    }

    //判断是否已满
    bool full(){
        m_mutex.lock();
        if(m_size>=m_max_size){
            m_mutex.unlock();
            return true;
        }
        m_mutex.unlock();
        return false;
    }

    //判断是否为空
    bool empty(){
        m_mutex.lock();
        if(m_size==0){
            m_mutex.unlock();
            return true;
        }
        m_mutex.unlock();
        return false;
    }

    //返回队首元素
    bool front(T&value){//引用传入参数，当值改变 调用该函数的原value值也改变
        m_mutex.lock();
        if(m_size==0){
            m_mutex.unlock();
            return false;
        }
        value=m_array[m_front];
        m_mutex.unlock();
        return true;
    }

    //返回队尾元素
    bool back(T&value){
        m_mutex.lock();
        if(m_size==0){
            m_mutex.unlock();
            return false;
        }
        value=m_array[m_back];
        m_mutex.unlock();
        return true;
    }

    int size(){
        int tmp=0;
        m_mutex.lock();
        tmp=m_size;
        m_mutex.unlock();
        return tmp;
    }

    int max_size(){
        int tmp=0;
        m_mutex.lock();
        tmp=m_max_size;
        m_mutex.unlock();
        return tmp;
    }

    //往队列中添加元素，需将所有使用队列先唤醒
    //当有元素push进队列，相当于生产者生产了一件元素
    //若当前没有线程等待条件变量，则唤醒无意义

    bool push(const T&item){
        m_mutex.lock();
        if(m_size>=m_max_size){
            m_cond.boardcast();
            m_mutex.unlock();
            return false;
        }

        m_back=(m_back+1)%m_max_size;
        m_array[m_back]=item;
        m_size++;
        m_cond.boardcast();
        m_mutex.unlock();
        return true;
    }

    //pop时，如果当前队列没有元素，将会等待条件变量
    bool pop(T&item){
        m_mutex.lock();
        while(m_size<=0){
            if(!m_cond.wait(m_mutex.get())){//参数是m_mutex类型的指针，wait自动上锁解锁
                m_mutex.unlock();//如果信号量为0则返回false
                return false;
            }
        }
        m_front=(m_front+1)%m_max_size;
        item=m_array[m_front];
        m_size--;
        m_mutex.unlock();
        return true;
    }

    //增加了超时处理
    bool pop(T&item,int ms_timeout){
        struct timespec t={0,0};//时间间隔初始化为0
        struct timeval now={0,0};//时间间隔初始化为0
        gettimeofday(&now,NULL);//获取当前的时间
        m_mutex.lock();
        if(m_size<=0){
            t.tv_sec=now.tv_sec+ms_timeout/1000;//秒数加上毫秒的每满1000的转换
            t.tv_nsec=(ms_timeout%1000)*1000000;//将毫秒不满1000的尾数部分转换成纳秒
            if(!m_cond.timewait(m_mutex.get(),t)){
                m_mutex.unlock();
                return false;
            }
        }

        if(m_size<=0){//做第二次判断表示在m_cond.timewait()返回后的一段时间内仍没有数据添加到当前队列
            m_mutex.unlock();
            return false;
        }

        m_front=(m_front+1)%m_max_size;
        item=m_array[m_front];
        m_size--;
        m_mutex.unlock();
        return true;
    }
    
};
