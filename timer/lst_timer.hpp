#pragma once
#include<time.h>
#include"../log/log.h"


struct client_data;

class util_timer//时间类实用工具
{    
public:
    util_timer():prev(NULL),next(NULL){}

public:
    time_t expire;//time_t类型的终止
    void(*cb_func)(client_data*);
    client_data*user_data;
    util_timer*prev;
    util_timer*next;
};


//客户端数据结构体
struct client_data{
    sockaddr_in address;
    int sockfd;
    util_timer*timer;
};

//排序时间列表
class sort_timer_lst
{
private:
    util_timer*head;
    util_timer*tail;

private:
    //添加数据到链表中
    void add_timer(util_timer*timer,util_timer*lst_head){
        util_timer*prev=lst_head;
        util_timer*tmp=prev->next;
        while(tmp){
            if(timer->expire<tmp->expire){
                prev->next=timer;
                timer->next=tmp;
                timer->prev=timer;
                tmp->prev=prev;
                break;
            }
            prev=tmp;
            tmp=tmp->next;
        }
        if(!tmp){
            prev->next=timer;
            timer->next=NULL;
            timer->prev=prev;
            tail=timer;
        }
    }
public:
    //构造函数初始化
    sort_timer_lst():head(NULL),tail(NULL){}
    //删光时间链表所有数据
    ~sort_timer_lst(){
        util_timer*tmp=head;
        while(tmp){
            head=tmp->next;
            delete tmp;
            tmp=head;
        }
    }
    void add_timer(util_timer*timer){
        if(!timer){
            return;
        }
        if(!head){
            head=tail=timer;
            return;
        }
        if(timer->expire<head->expire){
            timer->next=head;
            head->prev=timer;
            head=timer;
            return;
        }
        add_timer(timer,head);//private里的
    }
    //调整链表中的顺序
    void adjust_timer(util_timer*timer){
        if(!timer){
            return;
        }
        util_timer*tmp=timer->next;
        //如果timer为最后一个或者小于后一个则不需要调整
        if(!tmp||(timer->expire<tmp->expire)){
            return;
        }
        if(timer==head){//如果要调整的是头结点
            head=head->next;//新的头结点指向原头结点的下一个节点
            head->prev=NULL;//新头结点orev指针指向空
            timer->next=NULL;//timer也就是原头结点的next指向空
            add_timer(timer,head);//将切割下来的原头结点调用函数从新加入
        }
        else{
            //先把timer从链表中取出
            timer->prev->next=timer->next;
            timer->next->prev=timer->prev;
            //然后调佣add_timer函数再加进去
            add_timer(timer,timer->next);
        }
    }
    //删除链表中的节点
    void del_timer(util_timer*timer){
        if(!timer){
            return;
        }
        //如果链表里只有一个节点
        if((timer==head)&&(timer==tail)){
            delete timer;
            head=NULL;
            tail=NULL;
            return;
        }
        if(timer==head){
            head=head->next;
            head->prev=NULL;
            delete timer;
            return;
        }
        if(timer==tail){
            tail=tail->prev;
            tail->next=NULL;
            delete timer;
            return;
        }
        timer->prev->next=timer->next;
        timer->next->prev=timer->prev;
        delete timer;
    }
    void tick(){//打钩 调用
        if(!head){
            return;
        }
        LOG_INFO("%s", "timer tick");
        Log::get_instance()->flush();
        time_t cur=time(NULL);//获取现在的时间
        util_timer*tmp=head;
        while(tmp){
            if(cur<tmp->expire){//如果当前时间<之前的时间
                break;
            }
            tmp->cb_func(tmp->user_data);//调用当前时间对应的回调函数
            head=tmp->next;//移到下一个位置
            if(head){
                head->prev=NULL;//设置一下头结点前面为空
            }
            delete tmp;
            tmp=head;
        }
    }
};