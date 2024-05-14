#include"sql_conn_pool.h"
#include<stdlib.h>
#include<pthread.h>
#include <mysql/mysql.h>
#include <string>
#include <string.h>
#include <list>
#include <iostream>

using namespace std;

//构造函数初始化
sql_connection_pool::sql_connection_pool(){
    this->Curconn=0;
    this->Freeconn=0;
}

//获取sql_connection_pool的一个实例  是单例模式
sql_connection_pool* sql_connection_pool::getInstance(){
    static sql_connection_pool connpool;
    return &connpool;
}

//构造初始化
void sql_connection_pool::init(string url,string User,string Password,string DBName,int Port,unsigned int MaxConn){
    this->url=url;
    this->User=User;
    this->Password=Password;
    this->Port=Port;
    this->DatabaseName=DBName;

    lock.lock();
    for(int i=0;i<MaxConn;++i){
        MYSQL*conn=NULL;
        conn=mysql_init(conn);
        if(conn==nullptr){
            cout<<"Error: "<<mysql_error(conn);
            exit(1);
        }
        conn=mysql_real_connect(conn,url.c_str(),User.c_str(),Password.c_str(),DBName.c_str(),Port,NULL,0);
        if(conn==nullptr){
            cout<<"Error: "<<mysql_error(conn);
            exit(1);
        }
        connList.push_back(conn);
        ++Freeconn;
    }
    reserve=sem(Freeconn);
    this->Maxconn=Freeconn;
    lock.unlock();
}

//当有请求时，从数据库连接池中返回一个可用连接，更新使用和空闲连接数
MYSQL*sql_connection_pool::getConnection(){
    MYSQL*conn=NULL;
    if(connList.size()==0){
        return NULL;
    }
    reserve.wait();
    lock.lock();

    conn=connList.front();
    connList.pop_front();
    --Freeconn;
    ++Curconn;
    lock.unlock();
    return conn;
}

//释放当前使用的连接
bool sql_connection_pool::releaseConnection(MYSQL*conn){
    if(conn==nullptr){
        return false;
    }
    lock.lock();
    connList.push_back(conn);
    ++Freeconn;
    --Curconn;
    lock.unlock();
    reserve.post();
    return true;
}

//销毁数据库连接池
void sql_connection_pool::destroyPool(){
    lock.lock();
    if(connList.size()>0){
        for(list<MYSQL*>::iterator it=connList.begin();it!=connList.end();++it){
            MYSQL*conn=*it;
            mysql_close(conn);
        }
        Curconn=0;
        Freeconn=0;
        connList.clear();
    }
    lock.unlock();
}

//当前空闲的连接数
int sql_connection_pool::getFreeConn(){
    return this->Freeconn;
}

//析构函数的类外实现
sql_connection_pool::~sql_connection_pool(){
    destroyPool();
}

//集中构造和析构的类外实现
//获取一个可用连接
connectionRAII::connectionRAII(MYSQL**sql,sql_connection_pool*connPoll){
    *sql=connPoll->getConnection();
    connRAII=*sql;
    poolRAII=connPoll;
}
//只是释放当前连接
connectionRAII::~connectionRAII(){
    poolRAII->releaseConnection(connRAII);
}