#include<unistd.h>
#include<signal.h>
#include<sys/types.h>
#include<sys/epoll.h>
#include<fcntl.h>
#include<sys/socket.h>
#include<netinet/in.h>
#include<arpa/inet.h>
#include<assert.h>
#include<sys/stat.h>
#include<pthread.h>
#include<stdio.h>
#include<stdlib.h>
#include<sys/mman.h>
#include<stdarg.h>
#include<errno.h>
#include<sys/wait.h>
#include<sys/uio.h>
#include"../lock/lock.h"
#include"../CGImysql/sql_conn_pool.h"

//http_conn连接类
class http_conn
{
public:
    http_conn(){}
    ~http_conn(){}

public:
    static const int FILENAME_LEN=200;//文件名称的长度
    static const int READ_BUFFER_SIZE=2048;//读缓冲区的大小
    static const int WRITE_BUFFER_SIZE=1024;//写缓冲区的大小
    enum METHOD{//方法编号
        GET=0,
        POST,
        HEAD,
        PUT,
        DELETE,
        TRACE,
        OPTIONS,
        CONNECT,
        PATH
    };
    enum CHECK_STATE{//服务器返回检查状态
        CHECK_STATE_REQUESTLINE=0,//请求行
        CHECK_STATE_HEADER,//请求头部
        CHECK_STATE_CONTENT//请求内容
    };
    enum HTTP_CODE{
        NO_REQUEST,//第一个默认为0（请求不完整，需要继续读取请求报文数据）
        GET_REQUEST,//获得完整的http请求
        BAD_REQUEST,//请求语法出错
        NO_RESOURCE,//没有资源
        FORBIDDEN_REQUEST,//禁止请求
        FILE_REQUEST,//文件请求
        INTERNAL_ERROR,//内部错误
        CLOSED_CONNECTION//关闭连接
    };
    enum LINE_STATUS{
        LINE_OK,//完整读取一行
        LINE_BAD,//报文语法有误
        LINE_OPEN//线状态开放，表示还可以传输数据,读取的行不完整
    };

public:
    void init(int sockfd,const sockaddr_in &addr);
    void close_conn(bool real_close=true);
    void process();
    bool read_once();
    bool write();
    sockaddr_in*get_address(){
        return &m_address;
    }
    void initmysql_result(sql_connection_pool*connPool);//初始化数据库集
private:
    void init();//初始化
    HTTP_CODE process_read();//读过程
    bool process_write(HTTP_CODE ret);//写过程
    HTTP_CODE parse_request_line(char*text);//解析请求行
    HTTP_CODE parse_headers(char*text);//解析头部
    HTTP_CODE parse_content(char*text);//解析内容
    HTTP_CODE do_request();//执行请求
    char*get_line(){return m_read_buf+m_start_line;};//m_start_line是行在buffer中的起始位置
    LINE_STATUS parse_line();//解析一行
    void unmap();//删除map容器
    bool add_response(const char*format,...);//增加回答
    bool add_content(const char*content);//增添内容
    bool add_status_line(int status,const char*title);//增添行状态
    bool add_headers(int content_length);//增添头部
    bool add_content_type();//增添内容类型
    bool add_content_length(int content_length);//增添内容长度
    bool add_linger();//是否连接
    bool add_blank_line();//增添空白行

public:
    static int m_epollfd;//epoll文件描述符
    static int m_user_count;//客户端数量
    MYSQL*mysql;//数据库指针

private:
    int m_sockfd;
    sockaddr_in m_address;
    char m_read_buf[READ_BUFFER_SIZE];//读缓冲区
    int m_read_idx;//当前读取的下标
    int m_checked_idx;//已经读取的下标
    int m_start_line;//开始的行
    char m_write_buf[WRITE_BUFFER_SIZE];//写缓冲区
    int m_write_idx;//当前写的下标
    CHECK_STATE m_check_state;//检查状态
    METHOD m_method;//方法
    char m_real_file[FILENAME_LEN];//文件名缓冲区
    char*m_url;//网址
    char*m_version;//版本
    char*m_host;//主用户
    int m_content_length;//内容长度
    bool m_linger;
    char*m_file_address;//文件地址
    struct stat m_file_stat;//文件状态
    struct iovec m_iv[2]; //多元素数组
    int m_iv_count; //元素的数量
    int cgi;//是否启用POST
    char*m_string;//存储请求头部数据
    int bytes_to_send;//将要发送的字节数
    int bytes_have_send;//已经发送的字节数
};
