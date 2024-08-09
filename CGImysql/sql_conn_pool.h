#pragma once
#include<iostream>
#include<list>
#include<mysql/mysql.h>
#include<error.h>
#include<string.h>
#include<stdio.h>
#include<string>
#include"../lock/lock.h"

using namespace std;


class sql_connection_pool
{
private:
    unsigned int Maxconn;//最大连接数
    unsigned int Curconn;//当前已使用连接数
    unsigned int Freeconn;//当前空闲的连接数

private:
    locker lock;//locker类
    list<MYSQL*>connList;//连接池
    sem reserve; //PV操作 队列状态，是否需要处理

private:
    string url;//主机地址
    string Port;//数据库端口号
    string User;//数据库用户名
    string Password;//登录数据库密码
    string DatabaseName;//使用数据库名

private:
    //构造与析构函数
    sql_connection_pool();
    ~sql_connection_pool();

    MYSQL*getConnection();//获取数据库中一个可用的连接连接
    bool releaseConnection(MYSQL*conn);//释放当前使用的连接
    int getFreeConn();//获取当前空闲连接数量
    void destroyPool();//销毁数据库连接池

    //单列模式
    static sql_connection_pool*getInstance();

    void init(string url,string User,string Password,string DatabaseName,int Port,unsigned int Maxconn);

};
//集中构造和析构函数
class connectionRAII
{
private:
    MYSQL*connRAII;
    sql_connection_pool*poolRAII;
public:
    connectionRAII(MYSQL**conn,sql_connection_pool*connPool);
    ~connectionRAII();
};

