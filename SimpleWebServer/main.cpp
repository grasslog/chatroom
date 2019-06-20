#include<sys/socket.h>
#include<netinet/in.h>
#include<arpa/inet.h>
#include<stdio.h>
#include<unistd.h>
#include<errno.h>
#include<string.h>
#include<fcntl.h>
#include<stdlib.h>
#include<cassert>
#include<sys/epoll.h>

#include"locker.h"
#include"threadpool.h"
#include"http_conn1.h"

#define MAX_FD 65536
#define MAX_EVENT_NUMBER 10000


extern void addfd(int epollfd,int fd,bool one_shot);
extern void removefd(int epollfd,int fd);

void addsig(int sig,void(handler)(int),bool restart = true){
    struct sigaction sa;
    memset(&sa,'\0',sizeof(sa));
    sa.sa_handler = handler;
    if(restart){
        sa.sa_flags |= SA_RESTART;
    }
    sigfillset(&sa.sa_mask);
    assert(sigaction(sig,&sa,NULL) != -1);
}

void show_error(int connfd,const char* info){
    send(connfd,info,strlen(info),0);
    close(connfd);
}

int main(int argc,char* argv[]){
    if(argc <= 2){//argv[0]可执行文件名/main,argv[1]IP地址，argv[2]是端口号
        printf("usage: %s ip_address port_number\n",basename(argv[0]));//最后一个/的字符串内容
        return 1;
    }
    const char* ip = argv[1];
    char* port = argv[2];

    //忽略SIGPIPE信号
    addsig(SIGPIPE,SIG_IGN);//SIG_IGN表示忽略SIGPIPE那个注册的信号。

    //创建线程池
    threadpool<http_conn>* pool = NULL;
    try{//这里的语句有任何异常就执行下面的return
        pool = new threadpool<http_conn>;        
    }
    catch(...){
        return 1;
    }

    //预先为每个可能的客户连接分配一个http_conn对象
    http_conn* users = new http_conn[MAX_FD];
    assert(users);
    int user_count = 0;

    int listenfd = socket(PF_INET,SOCK_STREAM,0);
    assert(listenfd >= 0);
    struct linger tmp = {1,0};//1表示还有数据没发送完毕的时候容许逗留，0表示逗留时间
    setsockopt(listenfd,SOL_SOCKET,SO_LINGER,&tmp,sizeof(tmp));//即让没发完的数据发送出去后在关闭socket

    int ret = 0;
    struct sockaddr_in address;
    bzero(&address,sizeof(address));
    address.sin_family = AF_INET;
    //address.sin_addr.s_addr = htonl(ip);
    inet_aton(ip,&address.sin_addr);
    address.sin_port = htons(atoi(port));

    ret = bind(listenfd,(struct sockaddr*)& address,sizeof(address));
    assert(ret >= 0);

    ret = listen(listenfd,5);
    epoll_event events[MAX_EVENT_NUMBER];
    int epollfd = epoll_create(5);
    assert(epollfd != -1);
    addfd(epollfd,listenfd,false);
    http_conn::m_epollfd = epollfd;

    while(true){
        int number = epoll_wait(epollfd,events,MAX_EVENT_NUMBER,-1);
        if((number < 0) && (errno != EINTR)){
            printf("epoll failure\n");
            break;
        }
        
        for(int i = 0;i < number;++i){
            int sockfd = events[i].data.fd;
            if(sockfd == listenfd){
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof(client_address);                
                int connfd = accept(listenfd,(struct sockaddr*)& client_address,&client_addrlength);
                if(connfd < 0){
                    printf("error is: %d\n",errno);
                    continue;
                }
                if(http_conn::m_user_count >= MAX_FD){
                    show_error(connfd,"Internal server busy");
                    continue;
                }
                //初始化客户连接
                users[connfd].init(connfd,client_address);
                printf("sock_close\n");
            }
            else if(events[i].events & (EPOLLRDHUP | EPOLLHUP |EPOLLERR)){
                //如果有异常，直接关闭客户连接
                printf("sock_exception_close\n");
                users[sockfd].close_conn();
            }
            else if(events[i].events & EPOLLIN){
                //根据读的结果，决定将任务添加到线程池，还是关闭连接
                if(users[sockfd].read()){
                    pool->append(users + sockfd);
                }
                else{
                    printf("sock_read_close\n");
                    users[sockfd].close_conn();
                }
            }
            else if(events[i].events & EPOLLOUT){
                //根据写的结果，决定是否关闭连接
                printf("write_main\n");
                if(!users[sockfd].write()){
                    printf("sock_write_close\n");
                    users[sockfd].close_conn();
                }
            }
            else
            {printf("close\n");}
        }

    }

    close(epollfd);
    close(listenfd);
    delete [] users;
    delete pool;
    return 0;



}

main.cpp