#ifndef NGINX_MEM_H
#define NGINX_MEM_H
#include <cstdio>


using u_char = unsigned char;
using u_int = unsigned int;
using ngx_pool_cleanup_pt = void(*)(void *data);


struct ngx_pool;

/* 资源清理节点结构 */
struct ngx_pool_cleanup
{
    ngx_pool_cleanup_pt handler; // 清理函数指针（如关闭文件/释放资源）
    void *data;                  // 传递给清理函数的参数（如文件描述符）
    ngx_pool_cleanup *next;      // 指向下一个清理节点（形成清理链表）
};

/* 大内存块管理结构 */
struct ngx_pool_large
{
    ngx_pool_large *next; // 链表指针（连接多个大内存块）
    void *alloc;          // 实际分配的内存地址（通过malloc分配）
};

/* 小块内存池数据区块 */
struct ngx_pool_data
{
    u_char *last;   // 当前可用内存起始位置（分配时从此开始）
    u_char *end;    // 当前内存池结束地址（last到end为可用空间）
    ngx_pool *next; // 指向下一个内存池节点（形成内存池链表）
    u_int failed;   // 当前内存池分配失败次数（用于优化遍历）
};
/* 核心内存池结构 */
struct ngx_pool
{
    ngx_pool_data d;
    u_int max;
    ngx_pool *current;
    ngx_pool_large *large;
    ngx_pool_cleanup *cleanup;
};

class Nginx_mem
{

public:
    Nginx_mem(size_t size);
    ~Nginx_mem();

    void *ngx_pnalloc(size_t size);
    void *ngx_palloc(size_t size);
    void *ngx_pcalloc(size_t size);

    bool ngx_pfree(void *p);

    void ngx_reset_pool();

private:
    int ngx_create_pool(size_t size);

    void ngx_destroy_pool();

    void *ngx_palloc_small(size_t size, bool align);
    void *ngx_palloc_block(size_t size);

    void *ngx_palloc_large(size_t size);

    ngx_pool_cleanup *ngx_pool_cleanup_add(size_t size);

private:
    const int NGX_MAX_ALLOC_FROM_POOL;
    const int NGX_ALIGNMENT;
    ngx_pool *pool_;
};

#endif