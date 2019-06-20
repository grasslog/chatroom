#pragma once

//定义http响应的一些状态信息
const char* ok_200_title = "OK";
const char* error_400_title = "Bad Request";
const char* error_400_form = "You request has had syntax or is inherently impossible to satisfy.\n";
const char* error_403_title = "Forbidden";
const char* error_403_form = "You do not have permission to get file from this server.\n";
const char* error_404_title = "Not Found";
const char* error_404_form = "The requested file was not found on this server.\n";
const char* error_500_title = "Internal Error";
const char* error_500_form = "There was an unusual problem serving the requested file.\n"
//网站的一些根目录
const char* doc_root = "/var/www/html";

int setnonblocking(int fd){//将文件描述符设置为非阻塞(边缘触发搭配非阻塞)
    int old_option = fcntl(fd,F_GETFL);
    int new_option = old_option| O_NONBLOCK;
    fcntl(fd,F_SETFL,new_option);
    return old_option;
}

void addfd(int epollfd,int fd,bool one_shot){//向epoll例程中注册监视对象文件描述符
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLET | EPOLLRDHUB;//注册三种事件类型
    if(one_shot){
        event.events |= EPOLLONESHOT;
    }
    epoll_ctl(epollfd,EPOLL_CTL_ADD,fd,&event);
    setnonblocking(fd);
}

void removefd(int epollfd,int fd){//移除并关闭文件描述符
    epoll_ctl(epollfd,EPOLL_CTL_DEL,fd,0);
    close(fd);
}

void modfd(int epollfd,int fd,int ev){//重置事件，更改文件描述符，可以接受ev对应的读/写/异常事件
    epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLEDHUB;
    epoll_cet(epollfd,EPOLL_CTL_MOD,fd,&event);
}

int http_conn :: m_user_count = 0;//用户数量
int http_conn :: m_epollfd = -1;

void http_conn :: close_conn(bool real_close){
    if(real_close && (m_sockfd != -1)){
        removefd(m_epollfd,m_sockfd);
        m_sockfd = -1;
        m_user_count--;//关闭一个连接时，将客户总量减1
    }
}

void http_conn :: init(int sockfd,const sockaddr_in& addr){
    m_sockfd = sockfd;
    m_address = addr;
    //以下两行为了避免TIME_WAIT状态
    int reuse = 1;
    setsockopt(m+sockfd,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));
    addfd(m_epollfd,sockfd,true);
    m_user_count++;

    init();
}

void http_conn::init(){
    m_check_state = CHECK_STATE_REQUESTLINE;
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
    memset(m_read_buf,'\0',READ_BUFFER_SIZE);
    memset(m_write_buf,'\0',WRITE_BUFER_SIZE);
    memset(m_real_file,'\0',FILENAME_LEN);
}
//从状态机=>得到行的读取状态，分别表示读取一个完整的行，行出错，行的数据尚且不完整
http_conn::LINE_STATUS http_conn::parse_line(){
    char temp;
    /*m_checked_idx指向buffer中当前正在分析的字节，m_read_idx指向buffer中客户数据的尾部的下一字节。
    buffer中[0~m_checked_idx - 1]都已经分析完毕，下面的循环分析[m_checked_idx~m_read_idx- 1]的数据*/
    for(;m_checked_idx < m_read_idx;++m_checked_idx){
        //获取当前要分析的字节
        temp = m_read_buf[m_checked_idx];
        //如果当前字符是‘\t’则有可能读取一行
        if(temp == '\r'){
            //如果\t是buffer中最后一个已经被读取的数据，那么当前没有读取一个完整的行，还需要继续读
            if(m_checked_idx + 1 == m_read_idx){
                return LINE_OPEN;
            }
            //如果下一个字节是\n，那么已经读取到完整的行
            else if(m_read_buf[m_checked_idx + 1] == '\n'){
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            //否则的话，说明客户发送的HTTP请求存在语法问题
            return LINE_BAD;
        }
        //如果当前的字节是\n，也说明可能读取到一个完整的行
        else if(temp == '\n'){
            if(m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r'){
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            else {
                return LINE_BAD;
            }
        }
    }
    //如果所有内容分析完毕也没遇到\t字符，则还需要继续读取客户数据
    return LINE_OPEN;
}

//循环读取客户数据，直到无数据可读或者对方关闭连接
bool http_conn::read(){
    if(m_read_idx >= READ_BUFFER_SIZE){
        return false;
    }
    while(true){
        bytes_read = recv(m_sockfd,m_read_buf + m_read_idx,READ_BUFFER_SIZE - m_read_idx,0);
        if(bytes_read == -1){
            if(errno == EAGAIN || errno == EWOULDBLOCK){
                break;
            }
            return false;
        }
        else if(bytes_read == 0){
            return false;
        }
        m_read_idx += bytes_read;
    }
    return true;    
}

//解析HTTP请求行，获得请求方法,目标URL，以及HTTP版本号
// GET http://www.google.com:80/index.html HTTP/1.1
http_conn :: HTTP_CODE http_conn::parse_request_line(char* text){
    m_url = strpbrk(text,"\t");//A找到第一个等于空格或者制表符的下标位置GET
    if(!m_url){
        return BAD_REQUEST;
    }
    *m_url++ = '\0';//GET\0

    char* method = text;
    if(strcasecmp(method,"GET") == 0){//忽略大小写比较大小
        m_method = GET;
    }
    else{
        return BAD_REQUEST;
    }

    m_url += strspn(m_url,"\t");//去掉GET后面多余空格的影响，找到其中最后一个空格位置
    m_version = strpbrk(m_url,"\t");//找到url的结束位置html和HTTP中间的空格位置
    if(!m_version){
        return BAD_REQUEST;
    }
    *m_version++ = '\0';//html\0HTTP
    m_version += strspn(m_version,"\t");//去掉中间空格，此时m_version指向H,从状态机中已经将每行的结尾设置为\0\0
    if（strcasecmp(m_version,"HTTP/1.1") != 0）{//仅支持HTTP/1.1
        return BAD_REQUEST;
    }
    if（strncmp(m_url,"HTTP://",7) == 0){//检查url是否合法
        m_url += 7;
        m_url = strchr(m_url,'/');//找/index中的/        
    }

    if(!m_url || m_url[0] != '/'){//记住URL后缀是/
        return NO_REQUEST;
    }
    //HTTP请求行处理完毕，状态转移到头部字段的分析
    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;
}

//分析头部字段
//HTTP请求的组成是一个请求行，后面跟随0个或者多个请求头，最后跟随一个空的文本行来终止报头列表
HTTP_CODE parse_headers(char* temp){
    //遇到空行，说明头部字段解析完毕
    if(text[0] == '\0'){
        //如果HTTP请求有消息体，则还需要读取m_content_length字节的消息体，状态机转移到CHECK_STATE_CONTENT状态
        if(m_content_length != 0){
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        //否则说明我们得到一个完整的HTTP请求
        return GET_REQUEST;
    }
    //下面处理多种请求报头
    //处理Connection头部字段
    else if(strncasecmp(text,"Connection:",11) == 0){
        text += 11;
        text += strspn(text,"\t");
        if(strcasecmp(text,"keep-alive") == 0){
            m_linger = true;
        }
    }
    //处理content-length头部字段
    else if(strcasecmp(text,"Content-Length:",15) == 0){
        text += 15;
        text += strspn(text,"\t");
        m_content_length = atol(text);
    }
    //处理Host头部信息
    else if(strncasecmp(text,"Host:",5) == 0){
        text += 5;
        text += strspn(text,"\t");
        m_host = text;
    }
    else{
        printf("oop!unkonwn header %s\n",text);
    }

    return NO_REQUEST;
}

//我们没有真正的解析HTTP请求的消息体，只是判断它是否被完整的读入了
http_conn::HTTP_CODE http_conn::parse_content(char* text){
    if(m_read_idx >= (m_content_length + m_checked_idx)){
        text[m_content_length] = '\0';
        return GET_REQUEST;
    }

    return NO_REQUEST;
}

//主状态机，用于从buffer中取出所有完整的行
http_conn::HTTP_CODE http_conn::process_read(){
    LINE_STATUS line_status = LINE_OK;//记录当前行的读取状态
    HTTP_CODE ret = NO_REQUEST;//记录HTTP请求的处理结果
    char* text = 0;

    while(((m_check_state == CHECK_STATE_CONTENT) && (line_status == LINE_OK)) 
        || (line_status = parse_line()) == LINE_OK) {//m_check_state记录主状态机当前的状态
         text = get_line();
         m_start_line = m_checked_idx;
         printf("got 1 http line:%s\n",text);

         switch(m_check_state){
             case CHECK_STATE_REQUEST:{//分析请求行
                 ret = parse_request_line(text);
                 if(ret == BAD_REQUEST){
                     return BAD_REQUEST;
                 }
                 break;
             }
             case CHECK_STATE_HEADER:{//分析头部字段
                ret = parse_header(text);
                if(ret == BAD_REQUEST){
                    return BAD_REQUEST;
                }
                else if(ret == GET_REQUEST){
                    return do_request();
                }
                break;
             }
             case CHECK_STATE_CONTENT:{//分析消息体
                 ret = parse_content(text);
                 if(ret == GET_REQUEST){
                     return do_request();
                 }
                 line_status = LINE_OPEN;
                 break;
             }
             default:{
                 return INTERNAL_ERROR;
             }

         }

    }

    return NO_REQUEST;
}

/*当得到一个完整正确的HTTP请求时，我们就分析目标文件的属性。如果目标文件存在，
对所有用户可读，且不是目录，则使用mmap将其映射内存地址m_file_address处，并告诉调用者获取文件成功*/
http_conn::HTTP_CODE http_conn::do_request(){
    strcpy(m_real_file,doc_root);
    int len = strlen(doc_root);
    //m_real_file客户请求的目标文件的完整路径，其内容等于doc_root + m_url,doc_root是网站根目录
    strncpy( m_real_file + len,m_url,FILENAME_LEN - len - 1);
    if(stat(m_real_file,&m_file_stat)){//获取文件的状态并保存在m_file_stat中
        return NO_RESOURCE;
    }
    if(!(m_file_stat.st_mode & S_TROTH)){
        return FORBIDDEN_REQUEST;
    }
    if(S_ISDIR(m_file_stat.st_mode)){
        return BAD_REQUEST;
    }

    int fd = open(m_real_file,O_RDONLY);
    //创建虚拟内存区域，并将对象映射到这些区域
    m_file_address = (char*)mmap(0,m_file_stat.st_size,PROT_READ,MAP_PRIVATE,fd,0);
    close(fd);
    return FILE_REQUEST;
}

//对内存映射区执行munmap操作
void http_conn::unmap(){
    if(m_file_address){
        munmap(m_file_address,m_file_stat.st_size);//删除虚拟内存的区域
        m_file_address = 0
    }
}

//写HTTP响应
bool http_conn::write(){
    int temp = 0;
    int bytes_have_send = 0;
    int bytes_to_send = m_write_idx;
    if(bytes_to_send == 0){
        modfd(m_epollfd,m_sockfd,EPOLLIN);
        init();
        return true;
    }

    while(1){
        temp = writev(m_sockfd,m_iv,m_iv_count);
        if(temp <= -1){
        //如果TCP写缓存没有空间，则等待下一轮EPOLLOUT事件。虽然在此期间，服务器无法立即接收到同一客户的下一个请求，但是可以保证连接的完整性
            if（errno == EAGAIN）{//当前不可写
                modfd(m_epollfd,m_sockfd,EPOLLOUT);
                return true;
            }
            unmap();
            return false;
        }

        bytes_to_send -= temp;
        bytes_have_send += temp;
        if(nytes_to_send <= bytes_have_send){
            //发送HTTP响应成功，根据HTTP请求中的Connection字段决定是否立即关闭连接
            unmap();
            if(m_linger){
                init();
                modfd(m_epollfd,m_sockfd,EPOLLIN);
                return true;
            }
            else{
                modfd(m_epollfd,m_sockfd,EPOLLIN);
                return false;
            }
        }
    }
}

//往写缓冲区写入待发送的数据
bool http_conn::add_response(const char*format,...){
    if(m_write_idx >= WRITE_BUFFER_SIZE){
        return false;
    }
    va_list arg_list;
    va_start(arg_list,format);
    //将可变参数格式化输出到一个字符数组
    int len = vsnprintf(m_write_buf + m_write_idx,WRITE_BUFFER_SIZE - 1 - m_write_idx,format,arg_list);
    if(len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx)){
        return false;
    }
    m_write_idx += len;
    va_end(arg_list);
    return true;
}

bool http_conn::add_status_line(int status,const char* title){
    return add_response("%s %d %s\r\n","HTTP/1.1",status,title);
}

bool http_conn::add_headers(int content_len){//头部就三种信息
    add_content_length(content_len);//内容长度
    add_linger();//客户连接信息
    add_blank_line();//空行
}

bool http_conn::add_content_length(int content_len){
    return add_response("Content-Length: %d\r\n",content_len);
}

bool http_conn::add_linger(){
    return add_response("Connection: %s\r\n",(m_linger == true) ? "keep-alive" : "close");
}

bool http_conn::add_blank_line(){
    return add_response("%s","\r\n");
}

bool http_conn::add_content(const char* content){
    return add_response("%s",content);
}

//根据服务器处理HTTP请求的结果，决定返回给客户端的内容
bool http_conn::process_write(HTTP_CODE ret){
    switch(ret){
        case INTERNAL_ERROR:{
            add_status_line(500,error_500_title);
            add_headers(strlen(error_500_form));
            if(!add_content(error_500_form)){
                return false;
            }
            break;
        }
        case BAD_REQUEST:{
            add_status_line(400,error_400_title);
            add_headers(strlen(error_400_form));
            if(!add_content(error_400_form)){
                return false;
            }
            break;
        }
        case NO_RESOURCE:{
            add_status_line(404,error_404_title);
            add_headers(strlen(error_404_form));
            if(!add_content(error_404_form)){
                return false;
            }
            break;
        }
        case FORBIDDEN_REQUEST:{
            add_status_line(403,error_403_title);
            add_headers(strlen(error_403_form));
            if(!add_content(error_403_form)){
                return false;
            }
            break;
        }
        case FILE_REQUEST:{
            add_status_line(200,ok_200_title);
            if(m_file_stat.st_size != 0){//st_size表示文件的大小
                add_headers(m_file_stat.st_size);
                m_iv[0].iov_base = m_write_buf;
                m_iv[0].iov_len = m_write_idx;
                m_iv[1].iov_base = m_file_address;
                m_iv[1].iov_len = m_file_stat.st_size;
                m_iv_count = 2;
                return true;
            }
            else{
                const char* ok_string = "<html><body></body></html>";
                add_headers(strlen(strlen(ok_string)));
                if(!add_content(ok_string)){
                    return false;
                }
            }
        }
        default:{
            return false;
        }
        m_iv[0].iov_base = m_write_buf;
        m_iv[0].iov_len = m_write_idx;
        m_iv_count = 1;
        return true;
    }
}

//有线程池中的工作线程调用，这是处理HTTP请求的入口函数
void http_conn::process(){
    HTTP_CODE read_ret = process_read();
    if(read_ret == NO_REQUEST){
        modfd(m_epollfd,m_sockfd,EPOLLIN);
        return;
    }

    bool write_ret = process_write(read_ret);
    if(!write_ret){
         close_conn();
    }
    modfd(m_epollfd,m_sockfd,EPOLLOUT);
}

http_conn.cpp