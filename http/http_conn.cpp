#include"http_conn.h"
#include"../log/log.h"
#include<map>
#include<mysql/mysql.h>
#include<fstream>


//#define connfdET //边缘触发非阻塞
#define connfdLT //水平触发阻塞

//#define listenfdET //监听描述符边缘触发非阻塞
#define listenfdLT //监听描述符水平触发阻塞

//定义http响应的一些状态信息
const char* ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to staisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file form this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the request file.\n";

//当浏览器出现连接重置时，可能是网站根目录出错或http响应格式出错或者访问的文件内容完全为空
const char*doc_root="/root/TinyWebServer/TinyWebServer-raw_version/TinyWebServer-raw_version/root";

//将表中的用户名和密码存入map容器
map<string,string> users;
locker m_lock;

//初始化数据库结果集
void http_conn::initmysql_result(sql_connection_pool*connPool){
    //先从连接池中取出一个链接
    MYSQL*mysql=NULL;
    connectionRAII mysqlcon(&mysql,connPool);
    //在user表中检索username，password数据，浏览器端输入
    if (!mysql_query(mysql, "SELECT username,passwd FROM user"))
    {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }

    //从表中检索完整的结果集,返回结果集指针
    MYSQL_RES*result=mysql_store_result(mysql);
    //返回结果集中的列数
    int num_fields=mysql_num_fields(result);
    //返回所有字段结构的数组
    MYSQL_FIELD*fields=mysql_fetch_fields(result);
    //从结果集中获取下一行，将对应的用户名和密码，存入map中
    while (MYSQL_ROW row = mysql_fetch_row(result))//mysql_fetch_row用于从result中获取下一行数据，返回的是MYSQL_ROW类型的指针，所以会一直执行直到下一行为空
    {
        string temp1(row[0]);//row[0]和row[1]代表当前行的第一列和第二列数据
        string temp2(row[1]);
        users[temp1] = temp2;//将用户名和密码在map容器中存储起来
    }
}

//将文件描述符设置为非阻塞
int setnonblocking(int fd){
    int old_opinion=fcntl(fd,F_GETFD);
    if(old_opinion<0){
        perror("fcntl get error:");
        exit(EXIT_FAILURE);
    }
    if(fcntl(fd,F_SETFD,old_opinion|O_NONBLOCK)<0){//将添加步骤和判断是否错误结合在一起
        perror("fcntl set error:");
        exit(EXIT_FAILURE);
    }
}

//将事件加入到epoll_event事件中,并选择开启EPOLLONESHOT模式
void addfd(int epollfd,int fd,bool one_shot){
    epoll_event event;
    event.data.fd=fd;

#ifdef connfdET
    event.events=EPOLLIN|EPOLLOUT|EPOLLHUP;
#endif

#ifdef connfdLT
    event.events=EPOLLIN|EPOLLRDHUP;
#endif

#ifdef listenfdEF
    event.events=EPOLLIN|EPOLLET|EPOLLRDHUP;
#endif

#ifdef listenfdLT
    event.events=EPOLLIN|EPOLLRDHUP;
#endif 

    if(one_shot){
        event.events|=EPOLLONESHOT;
    }
    epoll_ctl(epollfd,EPOLL_CTL_ADD,fd,&event);
    //设置fd为非阻塞
    setnonblocking(fd);
}

//从内核时间表删除文件描述符
void removefd(int epollfd,int fd){
    epoll_ctl(epollfd,EPOLL_CTL_DEL,fd,0);
    close(fd);
}

//将事件重置为EPOLLONESHOT
void modfd(int epollfd,int fd,int ev){
    epoll_event event;
    event.data.fd=fd;
#ifdef connfdET
    event.events=ev|EPOLLET|EPOLLONESHOT|UPEPOLLRDHUP;
#endif

#ifdef connfdLT
    event.events = ev | EPOLLONESHOT | EPOLLRDHUP;
#endif

    //修改fd的事件
    epoll_ctl(epollfd,EPOLL_CTL_MOD,fd,&event);
}

//客户总量
int http_conn::m_user_count = 0;
//连接
int http_conn::m_epollfd = -1;

//关闭连接，关闭一个连接，客户总量减一
void http_conn::close_conn(bool real_close){
    if (real_close && (m_sockfd != -1))
    {
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
}

//初始化连接,外部调用初始化套接字地址
void http_conn::init(int sockfd, const sockaddr_in &addr){
    m_sockfd = sockfd;
    m_address = addr;
    addfd(m_epollfd, sockfd, true);
    m_user_count++;
    init();
}

//初始化新接收的连接
//check_state默认为分析请求状态
void http_conn::init(){
    mysql = NULL;
    bytes_to_send = 0;
    bytes_have_send = 0;
    m_check_state = CHECK_STATE_REQUESTLINE;//check_state默认为分析请求行状态
    m_linger = false;
    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    cgi = 0;
    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);
}

//从状态机，用于分析出一行的内容
//返回值为行的读取状态，有LINE_OK,LINE_BAD,LINE_OPEN
http_conn::LINE_STATUS http_conn::parse_line(){
    //m_read_idx指向缓冲区m_read_buf的数据末尾的下一个字节
    //m_checked_idx指向从状态机当前正在分析的字节
    char temp;
    for(m_checked_idx=0;m_checked_idx<m_read_idx;++m_checked_idx){
        temp=m_read_buf[m_checked_idx];
        if(temp=='\r'){
            if((m_checked_idx+1)==m_read_idx){//一行未能全部读取完毕
                return LINE_OPEN;//读取的行不完整
            }
            else if(m_read_buf[m_checked_idx+1]=='\n'){//换行符\n表示已全部读取完
                m_read_buf[m_checked_idx++]='\0';//将\r和\n换成\0
                m_read_buf[m_checked_idx++]='\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
        else if(temp=='\n'){
            if(m_checked_idx>1&&m_read_buf[m_checked_idx-1]=='\r'){
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;//如果结束的位置不是\r也不是\n那么未读取完
}

//循环读取客户数据，直到无数据可读或是对方关闭连接
//非阻塞ET工作模式下，需要一次性将数据读完

bool http_conn::read_once(){
    if (m_read_idx >= READ_BUFFER_SIZE)//如果将要读取的数据下标大于读缓冲区
    {
        return false;
    }
    int bytes_read = 0;

#ifdef connfdLT
                                //从m_read_buf的第m_read_idx个位置开始的容器，容器大小为READ_BUFFER_SIZE 减掉 偏移的长度m_read_idx
    bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);//recv返回的是接收数据的byte，错误返回-1
    m_read_idx += bytes_read;//更新下标位置

    if (bytes_read <= 0)//等于0表示无东西可读或连接已关闭
                        //小于0表示出错
    {
        return false;
    }

    return true;

#endif

//非阻塞ET工作模式下，需要一次性将数据读完
#ifdef connfdET
    while (true){
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        if (bytes_read == -1){
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;//如果是EAGAIN或者EWOULDBLOCK则跳出循环
            return false;//否则返回错误
        }
        else if (bytes_read == 0){
            return false;
        }
        m_read_idx += bytes_read;
    }
    return true;
#endif
}

//解析http请求行，获得请求方法，目标url及http版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char *text){
    m_url = strpbrk(text, " \t");//查找空格或\t第一次出现的位置
    if (!m_url){
        return BAD_REQUEST;//HTTP请求报文有语法错误
    }
    *m_url++ = '\0';//先把\t变成\0然后再++
    char *method = text;
    if (strcasecmp(method, "GET") == 0)//比较字符串大小>为正<为负=为0
        m_method = GET;//比较时碰到\0就会停下来
    else if (strcasecmp(method, "POST") == 0)
    {
        m_method = POST;
        cgi = 1;
    }
    else
        return BAD_REQUEST;
    m_url += strspn(m_url, " \t");//跳过空格和\t
    m_version = strpbrk(m_url, " \t");//找到了版本号的第一个位置
    if (!m_version)
        return BAD_REQUEST;
    *m_version++ = '\0';//将\t改为\0
    m_version += strspn(m_version, " \t");//跳过空格和\t
    if (strcasecmp(m_version, "HTTP/1.1") != 0)//比较是否版本号
        return BAD_REQUEST;
    if (strncasecmp(m_url, "http://", 7) == 0)
    {
        m_url += 7;//如果是http://则将指针向后偏移7各单位
        m_url = strchr(m_url, '/');//在以\0为结尾的字符串中查找第一次出现/的字符位置
    }

    if (strncasecmp(m_url, "https://", 8) == 0)
    {
        m_url += 8;//多了个s
        m_url = strchr(m_url, '/');
    }

    if (!m_url || m_url[0] != '/')//如果m_url为空或者m_url的第一个字符不为/
        return BAD_REQUEST;//HTTP请求报文有语法错误
    //当url为/时，显示判断界面
    if (strlen(m_url) == 1)//strlen不包含\0
        strcat(m_url, "judge.html");//将两个字符串拼接在一起
    m_check_state = CHECK_STATE_HEADER;//请求行处理完毕，将主状态机转移处理请求头
    return NO_REQUEST;
}

//解析http请求的一个头部信息
http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{
    //判断是否是空行还是请求头
    if (text[0] == '\0')
    {
        //判断是否是POST还是GET(!=0为POST)
        if (m_content_length != 0)
        {
            //将状态改成请求消息体
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;//返回还未完整读取
        }
        return GET_REQUEST;//如果是GET则返回已经完整读取了数据
    }
    else if (strncasecmp(text, "Connection:", 11) == 0)
    {//connection连接是否保持活跃
        text += 11;
        text += strspn(text, " \t");//跳过空格和\t
        if (strcasecmp(text, "keep-alive") == 0)
        {
            m_linger = true;//连接为true
        }
    }
    else if (strncasecmp(text, "Content-length:", 15) == 0)
    {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);//将字符串转变成长整型
    }
    else if (strncasecmp(text, "Host:", 5) == 0)
    {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }
    else
    {
        //printf("oop!unknow header: %s\n",text);
        LOG_INFO("oop!unknow header: %s", text);
        Log::get_instance()->flush();//将错误写入日志
    }
    return NO_REQUEST;
}

//判断http请求是否被完整读入
http_conn::HTTP_CODE http_conn::parse_content(char *text)
{
    //m_read_idx指向缓冲区m_read_buf的数据末尾的下一个字节
    if (m_read_idx >= (m_content_length + m_checked_idx))
    {
        text[m_content_length] = '\0';//在消息体数据的结尾处添加\0
        //POST请求中最后为输入的用户名和密码类似：user=zhangsan&password=123456
        m_string = text;
        return GET_REQUEST;//完整读入http请求
    }
    return NO_REQUEST;//读入还不完整
}

//主状态机根据不同的状态分配不同的函数
http_conn::HTTP_CODE http_conn::process_read(){
    //初始化从状态机状态、HTTP请求解析结果
    LINE_STATUS line_status = LINE_OK;//完整读取一行
    HTTP_CODE ret = NO_REQUEST;//请求不完整，需要继续读取请求报文数据
    char *text = 0;
    
    //parse_line为从状态机的具体实现
    while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) || ((line_status = parse_line()) == LINE_OK))
    {//POST通过主状态机用check_state和line_status判断状态、GET只需看line_status是否=LINE_OK来判断
        text = get_line();//返回的是m_read_buf+m_start_line
        //m_read_buf和m_start_line开始时均为0
        //cout<<"text:"<<text<<endl;//测试

        //m_start_line是每一个数据行在m_read_buf中的起始位置
        //m_checked_idx表示从状态机在m_read_buf中读取的位置
        m_start_line = m_checked_idx;//更新m_start_line的位置
        LOG_INFO("%s", text);
        Log::get_instance()->flush();
        switch (m_check_state)
        {
        case CHECK_STATE_REQUESTLINE:
        {
            ret = parse_request_line(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            break;
        }
        //完整解析GET请求后，跳转到报文响应函数
        case CHECK_STATE_HEADER://头部请求
        {
            ret = parse_headers(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            else if (ret == GET_REQUEST)
            {
                return do_request();
            }
            break;
        }
        //完整解析POST请求后，跳转到报文响应函数
        case CHECK_STATE_CONTENT://消息体请求
        {
            ret = parse_content(text);
            if (ret == GET_REQUEST)
                return do_request();
            //解析完消息体即完成报文解析，避免再次进入循环，更新line_status
            line_status = LINE_OPEN;
            break;
        }
        default:
            return INTERNAL_ERROR;//都不是则返回内部错误
        }
    }
    return NO_REQUEST;
}

//执行请求
http_conn::HTTP_CODE http_conn::do_request(){
    strcpy(m_real_file, doc_root);//将存放素材的地址放入文件路径中去
    int len = strlen(doc_root);
    //printf("m_url:%s\n", m_url);
    const char *p = strrchr(m_url, '/');//查找在m_url中最后一次出现/的位置，因为可能前面有http://之类的

    //处理cgi
    if (cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3'))
    {

        //根据标志判断是登录检测还是注册检测
        char flag = m_url[1];//表示 / 后面的数字

        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/");
        strcat(m_url_real, m_url + 2);//不包含数字
        //测试
		//cout<<"m_url_real: "<<m_url_real<<endl;

        strncpy(m_real_file + len, m_url_real, FILENAME_LEN - len - 1);//-1是为了把\0放进去
        free(m_url_real);

        //将用户名和密码提取出来
        //user=123&password=123
        char name[100], password[100];
        int i;
        for (i = 5; m_string[i] != '&'; ++i)
            name[i - 5] = m_string[i];
        name[i - 5] = '\0';

        int j = 0;
        for (i = i + 10; m_string[i] != '\0'; ++i, ++j)//+10是因为password=共有10个字符
            password[j] = m_string[i];
        password[j] = '\0';

        //同步线程登录校验
        if (*(p + 1) == '3')//注册校验界面
        {
            //如果是注册，先检测数据库中是否有重名的
            //没有重名的，进行增加数据
            char *sql_insert = (char *)malloc(sizeof(char) * 200);
            strcpy(sql_insert, "INSERT INTO user(username, passwd) VALUES(");//将后面的字符拷贝进 sql_insert 容器中去
            strcat(sql_insert, "'");
            strcat(sql_insert, name);
            strcat(sql_insert, "', '");
            strcat(sql_insert, password);
            strcat(sql_insert, "')");//先全部拼接到名为sql_insert的字符串缓冲区中

            if (users.find(name) == users.end())//如果没有在数据库中找到相匹配的名字
            {

                m_lock.lock();
                int res = mysql_query(mysql, sql_insert);//数据库指针和字符串
                users.insert(pair<string, string>(name, password));
                m_lock.unlock();

                if (!res)
                    strcpy(m_url, "/log.html");
                else
                    strcpy(m_url, "/registerError.html");
            }
            else
                strcpy(m_url, "/registerError.html");
        }

        //如果是登录，直接判断
        //若浏览器端输入的用户名和密码在表中可以查找到，返回1，否则返回0
        else if (*(p + 1) == '2')//登录校验界面
        {
            if (users.find(name) != users.end() && users[name] == password)
                strcpy(m_url, "/welcome.html");
            else
                strcpy(m_url, "/logError.html");
        }
    }

    if (*(p + 1) == '0')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/register.html");//开始界面
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '1')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/log.html");//登录界面
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '5')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/picture.html");//图片界面
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '6')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/video.html");//音视频界面
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '7')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/fans.html");//粉丝界面
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
     else
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);

    if (stat(m_real_file, &m_file_stat) < 0)//判断文件状态(失败)
        return NO_RESOURCE;
    if (!(m_file_stat.st_mode & S_IROTH))//S_IROTH表示其他用户是否有读取权限没有则为0
        return FORBIDDEN_REQUEST;
    if (S_ISDIR(m_file_stat.st_mode))//用于检查一个给定的st_mode 成员是否表示一个目录
        return BAD_REQUEST;//是的话返回1 不是的话返回0
    int fd = open(m_real_file, O_RDONLY);//以只读方式打开文件
                            //映射区起始位置、文件大小、映射区可读、映射区私有标志、文件描述符、偏移量
    m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    //m_file_address接受mmap返回的映射区域的指针
    close(fd);
    return FILE_REQUEST;
}

//删除mmap
void http_conn::unmap(){
     if (m_file_address)
    {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}

//集合写函数 用到了mmap映射
bool http_conn::write()
{
    int temp = 0;

    if (bytes_to_send == 0)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN);//转换成读监听
        init();//初始化一大堆
        return true;
    }

    while (1)
    {               //文件描述符、iovec指针、iovec结构体数组的个数
        temp = writev(m_sockfd, m_iv, m_iv_count);

        if (temp < 0)
        {
            if (errno == EAGAIN)//再次尝试
            {
                modfd(m_epollfd, m_sockfd, EPOLLOUT);//写监听
                return true;
            }
            unmap();
            return false;
        }

        bytes_have_send += temp;
		//cout<<"bytes_have_send:"<<bytes_have_send<<endl;
		//cout<<"m_write_buf:"<<m_write_buf<<endl;
        bytes_to_send -= temp;
        //这部分代码调整发送缓冲区，以确保数据正确地被发送。如果已发送的数据量大于或等于第一个数据块的大小，
        //它会调整第二个数据块的起始位置和长度。否则，它会调整第一个数据块的起始位置和长度。
        if (bytes_have_send >= m_iv[0].iov_len)//如果已经发送的数据量大于第一个数据块的大小
        {
            m_iv[0].iov_len = 0;//m_write_idx表示当前数据块缓冲区数据的下一个位置
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);//m_write_idx从第二块数据块从零开始
            m_iv[1].iov_len = bytes_to_send;//将要发送的数据量   //bytes_have_send - m_write_idx相当于剪掉了第二块数据块多余的，返回第二块数据块起始位置
        }
        else
        {
            m_iv[0].iov_base = m_write_buf + bytes_have_send;//在bytes_have_send位置的缓冲区指针
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;//要发送数据的量-已发送数据的量
        }

        if (bytes_to_send <= 0)//如果无数据可发，调整为读监听
        {
            unmap();
            modfd(m_epollfd, m_sockfd, EPOLLIN);

            if (m_linger)
            {
                init();//如果连接则重新初始化
                return true;
            }
            else
            {
                return false;
            }
        }
    }
}

//增加相应报文的回答(大类)
bool http_conn::add_response(const char *format, ...)
{
    //如果当前数据块缓冲区数据下标位置大于写缓冲区的大小
    if (m_write_idx >= WRITE_BUFFER_SIZE)
        return false;//无法再写入
    va_list arg_list;
    va_start(arg_list, format);//使用 va_list 和 va_start 来处理可变参数列表
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);//返回写入的字符数
    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx))
    {
        va_end(arg_list);
        return false;
    }
    m_write_idx += len;
    va_end(arg_list);
    LOG_INFO("request:%s", m_write_buf);
    Log::get_instance()->flush();
    return true;
}

//增加状态行
bool http_conn::add_status_line(int status, const char *title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}
//增加头部
bool http_conn::add_headers(int content_len)
{
    add_content_length(content_len);
    add_linger();
    add_blank_line();
}
//增加消息体长度
bool http_conn::add_content_length(int content_len)
{
    return add_response("Content-Length:%d\r\n", content_len);
}
//增加消息体种类
bool http_conn::add_content_type()
{
    return add_response("Content-Type:%s\r\n", "text/html");
}
//增加连接情况
bool http_conn::add_linger()
{
    return add_response("Connection:%s\r\n", (m_linger == true) ? "keep-alive" : "close");
}
//增加空白行
bool http_conn::add_blank_line()
{
    return add_response("%s", "\r\n");
}
//增加消息体
bool http_conn::add_content(const char *content)
{
    return add_response("%s", content);
}

//分情况返回不同的消息(回应客户端的请求)
bool http_conn::process_write(HTTP_CODE ret)
{
    switch (ret)
    {
    case INTERNAL_ERROR:
    {
        add_status_line(500, error_500_title);
        add_headers(strlen(error_500_form));
        if (!add_content(error_500_form))
            return false;
        break;
    }
    case BAD_REQUEST:
    {
        add_status_line(404, error_404_title);
        add_headers(strlen(error_404_form));
        if (!add_content(error_404_form))
            return false;
        break;
    }
    case FORBIDDEN_REQUEST:
    {
        add_status_line(403, error_403_title);
        add_headers(strlen(error_403_form));
        if (!add_content(error_403_form))
            return false;
        break;
    }
    case FILE_REQUEST:
    {
        add_status_line(200, ok_200_title);
        if (m_file_stat.st_size != 0)
        {
            add_headers(m_file_stat.st_size);
            m_iv[0].iov_base = m_write_buf;
            m_iv[0].iov_len = m_write_idx;
            m_iv[1].iov_base = m_file_address;
            m_iv[1].iov_len = m_file_stat.st_size;
            m_iv_count = 2;
            bytes_to_send = m_write_idx + m_file_stat.st_size;//缓冲区中数据长度+映射区中数据长度
            return true;
        }
        else//如果请求的文件大小=0
        {
            const char *ok_string = "<html><body></body></html>";
            add_headers(strlen(ok_string));
            if (!add_content(ok_string))
                return false;
        }
    }
    default://都不是以上情况则：
        return false;
    }//请求出错只申请一个m_iv  出错了就没必要再映射
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}

void http_conn::process()
{
    HTTP_CODE read_ret = process_read();
    if (read_ret == NO_REQUEST)//请求不完整，需要继续读取请求报文数据
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        return;
    }
    bool write_ret = process_write(read_ret);
    if (!write_ret)
    {
        close_conn();//关闭连接
    }
    modfd(m_epollfd, m_sockfd, EPOLLOUT);//再次改为写监听
}