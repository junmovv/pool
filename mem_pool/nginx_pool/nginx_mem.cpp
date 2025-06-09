#include <cstdlib>
#include <cstring>

#include <cstdint>
#include "nginx_mem.h"

#define ngx_align(d, a) (((d) + (a - 1)) & ~(a - 1))

#define ngx_align_ptr(p, a) \
    (u_char *)(((uintptr_t)(p) + ((uintptr_t)a - 1)) & ~((uintptr_t)a - 1))

Nginx_mem::Nginx_mem(size_t size)
    : NGX_MAX_ALLOC_FROM_POOL(4095), NGX_ALIGNMENT(sizeof(unsigned long))

{
    ngx_create_pool(size);
}

Nginx_mem::~Nginx_mem()
{
    ngx_destroy_pool();
}

/**
 * @ingroup MemoryPool
 * @brief 创建内存池
 * @param size 初始内存池大小
 * @param log 日志对象指针
 * @return 成功返回true，失败返回false
 * @note 实际分配大小 = size + 内存池管理结构(ngx_pool_t)
 */
int Nginx_mem::ngx_create_pool(size_t size)
{
    pool_ = (ngx_pool *)malloc(size);
    if (nullptr == pool_)
    {
        return false;
    }
    pool_->d.last = (u_char *)pool_ + sizeof(ngx_pool);
    pool_->d.end = (u_char *)pool_ + size;
    pool_->d.next = nullptr;
    pool_->d.failed = 0;

    size = size - sizeof(ngx_pool);

    pool_->max = (size < NGX_MAX_ALLOC_FROM_POOL) ? size : NGX_MAX_ALLOC_FROM_POOL;

    pool_->current = pool_;
    pool_->large = nullptr;
    pool_->cleanup = nullptr;

    return true;
}

void Nginx_mem::ngx_destroy_pool()
{
    for (ngx_pool_cleanup *c = pool_->cleanup; c; c = c->next)
    {
        if (c->handler)
        {
            c->handler(c->data);
        }
    }
    for (ngx_pool_large *l = pool_->large; l; l = l->next)
    {
        if (l->alloc)
        {
            free(l->alloc);
        }
    }
    for (ngx_pool *p = pool_, *n = pool_->d.next; /* void */; p = n, n = n->d.next)
    {
        free(p);
        if (nullptr == n)
        {
            break;
        }
    }
}

void Nginx_mem::ngx_reset_pool()
{
    for (ngx_pool_large *l = pool_->large; l; l = l->next)
    {
        if (l->alloc)
        {
            free(l->alloc);
        }
    }
    for (ngx_pool *p = pool_; p; p = p->d.next)
    {
        p->d.last = (u_char *)p + sizeof(ngx_pool); // 这里有有浪费
        p->d.failed = 0;
    }
    pool_->current = pool_;
    pool_->large = nullptr;
}
/**
 * @ingroup MemoryAlloc
 * @brief 分配内存(非对齐)
 * @param pool 内存池指针
 * @param size 请求内存大小
 * @return 成功返回内存地址，失败返回NULL
 * @note 分配策略:
 *  - size ≤ pool->max: 小块内存对齐分配(ngx_palloc_small)
 *  - size > pool->max: 大内存分配(ngx_palloc_large)
 */
void *Nginx_mem::ngx_pnalloc(size_t size)
{
    if (size <= pool_->max)
    {
        return ngx_palloc_small(size, false);
    }

    return ngx_palloc_large(size);
}

/**
 * @ingroup MemoryAlloc
 * @brief 对齐分配内存 --- 地址返回的是对齐 ,申请的还是size的大小
 * @param pool 内存池指针
 * @param size 请求内存大小
 * @return 成功返回内存地址，失败返回NULL
 * @note 分配策略:
 *  - size ≤ pool->max: 小块内存对齐分配(ngx_palloc_small)
 *  - size > pool->max: 大内存分配(ngx_palloc_large)
 */
void *Nginx_mem::ngx_palloc(size_t size)
{
    if (size <= pool_->max)
    {
        return ngx_palloc_small(size, true);
    }

    return ngx_palloc_large(size);
}

/**
 * @ingroup MemoryAlloc
 * @brief 分配并清零内存
 * @param size 请求大小
 * @return 成功返回归零内存地址，失败返回NULL
 */
void *Nginx_mem::ngx_pcalloc(size_t size)
{
    void *p = ngx_palloc(size);
    if (p)
    {
        memset(p, 0, size);
    }
    return p;
}
/**
 * @ingroup MemoryFree
 * @brief 释放大内存
 * @param p 需释放的内存地址
 * @return NGX_OK成功, NGX_DECLINED未找到
 * @note 仅释放large链表中的内存[8](@ref)  节点还在 可以被新申请的大块内存借用
 */
bool Nginx_mem::ngx_pfree(void *p)
{
    for (ngx_pool_large *l = pool_->large; l; l = l->next)
    {
        if (p == l->alloc)
        {
            free(l->alloc);
            l->alloc = nullptr;
            return true;
        }
    }
    return false;
}

/**
 * @internal
 * @brief 小块内存分配核心函数
 * @param size 请求大小
 * @param align 是否内存对齐
 * @return 成功返回内存地址，失败返回NULL
 */
void *Nginx_mem::ngx_palloc_small(size_t size, bool align)
{
    u_char *m = nullptr;
    ngx_pool *p = pool_;

    do
    {
        m = p->d.last;
        if (align)
        {
            m = ngx_align_ptr(m, NGX_ALIGNMENT);
        }
        if (p->d.end - m >= size)
        {
            p->d.last = m + size;
            return m;
        }
        p = p->d.next;
    } while (p);

    return ngx_palloc_block(size); // 空间不足则创建新块
}
/**
 * @internal
 * @brief 创建新的内存池块
 * @param size 请求大小
 * @return 成功返回分配地址，失败返回NULL
 */
void *Nginx_mem::ngx_palloc_block(size_t size)
{
    size_t psize = (size_t)(pool_->d.end - (u_char *)pool_);
    u_char *m = (u_char *)malloc(psize);
    if (nullptr == m)
    {
        return nullptr;
    }
    ngx_pool *n = (ngx_pool *)m;
    n->d.end = m + psize;
    n->d.next = nullptr;
    n->d.failed = 0;

    m += sizeof(ngx_pool_data);
    m = ngx_align_ptr(m, NGX_ALIGNMENT);
    n->d.last = m + size;
    ngx_pool *p = pool_->current;
    for (; p->d.next; p = p->d.next)
    {
        if (p->d.failed++ > 4)
        {
            pool_->current = p->d.next;
        }
    }
    p->d.next = n; // 尾插
    return m;
}

/**
 * @internal
 * @brief 大内存分配函数
 * @param size 请求大小
 * @return 成功返回内存地址，失败返回NULL
 * @note 大内存直接调用ngx_alloc(即malloc)[9](@ref)
 */
void *Nginx_mem::ngx_palloc_large(size_t size)
{
    void *p = malloc(size);
    if (nullptr == p)
    {
        return nullptr;
    }
    int n = 0;
    ngx_pool_large *large = nullptr;
    /* 复用空闲large节点 */
    for (large = pool_->large; large; large = large->next)
    {
        if (nullptr == large->alloc)
        {
            large->alloc = p;
            return p; // 复用空闲节点
        }
        if (n++ >= 3)
        {
            break; // 搜索前4个节点
        }
    }
    /* 在小快内存上创建新large节点 */
    large = (ngx_pool_large *)ngx_palloc_small(sizeof(ngx_pool_large), true);
    if (nullptr == large)
    {
        free(p); // 分配失败需释放内存
        return NULL;
    }

    large->alloc = p;
    // 头插 更新节点
    large->next = pool_->large;
    pool_->large = large;
    return p;
}
/* 清理回调相关函数 */
/**
 * @ingroup Cleanup
 * @brief 添加清理回调
 * @param size 附加数据大小
 * @return 清理结构指针
 * @note 典型用例: 文件描述符自动关闭[6](@ref)
 */
ngx_pool_cleanup *Nginx_mem::ngx_pool_cleanup_add(size_t size)
{
    ngx_pool_cleanup *c = (ngx_pool_cleanup *)ngx_palloc(sizeof(ngx_pool_cleanup));

    if (nullptr == c)
    {
        return nullptr;
    }

    if (size)
    {
        c->data = ngx_palloc(size);
        if (nullptr == c->data)
        {
            return nullptr;
        }
    }
    else
    {
        c->data = nullptr;
    }

    c->handler = nullptr;
    // 头插法 更新节点
    c->next = pool_->cleanup;
    pool_->cleanup = c;
    return c;
}

int main()
{
    return 0;
}