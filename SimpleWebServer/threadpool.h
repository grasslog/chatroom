#pragma once

#include<list>
#include<cstdio>
#include<exception>
#include<pthread.h>
#include"locker.h"
/*线程池类，把它定义为模板类是为了代码复用，模板参数T是任务类*/
template<typename T>
class threadpool{
public:
  /*参数thread_number是线程池中线程的数量，max_requests是请求队列中最多允许的，等待处理的请求的数量*/
    threadpool(int thread_number = 8,int max_requests = 1000);
    ~threadpool();
    //往请求队列中添加任务
    bool append(T* request);

private:
    //工作线程运行的函数，它不断从工作队列中取出任务并执行之
    static void* worker(void* arg);
    void run();

private:
    int m_thread_number;//线程池中的线程数
    int m_max_requests; //请求队列中允许的最大请求数
    pthread_t* m_threads;//描述线程池的数组，其大小为m_thread_number
    std::list<T*> m_workqueue;//请求队列
    locker m_queuelocker;//保护请求队列的互斥锁
    sem m_queuestat;//是否有任务需要处理
    bool m_stop;//是否结束线程
};
template<typename T>
threadpool<T>::threadpool(int thread_number,int max_requests):
    m_thread_number(thread_number),m_max_requests(max_requests),
    m_stop(false),m_threads(NULL)

{    
    if(thread_number <= 0 || max_requests <= 0){
        throw std::exception();
    }
    m_threads = new pthread_t[m_thread_number];
    if(!m_threads){
        throw std::exception();
    }
    //创建thread_number个线程，并设置为分离线程
    for(int i = 0;i < thread_number;++i){
        printf("create the %dth thread\n",i + 1);
        if(pthread_create(&m_threads[i],NULL,worker,this) != 0){
            delete [] m_threads;
            throw std::exception();
        }
        if(pthread_detach(m_threads[i]) != 0){
            delete [] m_threads;
            throw std::exception();
        }
    }
    
}

template<typename T>
threadpool<T> :: ~threadpool(){
    delete [] m_threads;
    throw std:: exception();
}

template<typename T>
bool threadpool<T>::append(T* request){
    /*操作工作队列前一定要加锁，因为它被所有工作队列共享*/
    m_queuelocker.lock();
    if(m_workqueue.size() > m_max_requests){
        m_queuelocker.unlock();
        return false;
    }
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post();//保证队列非空
    return true;
}

template<typename T>
void* threadpool<T> :: worker(void* arg){//传入的是this
    threadpool* pool = (threadpool*)arg;//将void×指针转化为threadpool*指针
    pool -> run();//就是下面实现的run函数
    return pool;
}

template<typename T>
void threadpool<T>::run(){//消费者
    while(!m_stop){
        m_queuestat.wait();//执行p操作
        m_queuelocker.lock();//对工作队列操作钱加锁
        if(m_workqueue.empty()){
            m_queuelocker.unlock();
            continue;
        }
        T* request = m_workqueue.front();
        m_workqueue.pop_front();
        m_queuelocker.unlock();
        if(!request){
            continue;
        }
        request -> process();//任务中要有process处理函数
    }    
}


threadpool.h