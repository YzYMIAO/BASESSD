/**
 * @Author: S. Sharma <silentcat>
 * @Date:   2019-03-07T10:32:56-06:00
 * @Email:  silentcat@protonmail.com
 * @Last modified by:   silentcat
 * @Last modified time: 2019-03-09T15:23:25-06:00
 */

#include "ds_ring_buffer.h"

#include "ssd_types.h" /* BYTE/WORD/DWORD/QWORD */

struct DS_RING_BUFFER_CONFIG_S
{
    size_t capacity;

    void *(*mem_alloc)(size_t size);
    void *(*mem_calloc)(size_t blocks, size_t size);
    void (*mem_free)(void *block);
};

struct DS_RING_BUFFER_S
{
    size_t size, capacity;
    size_t head, tail;
    QWORD *buf;

    void *(*mem_alloc)(size_t size);
    void *(*mem_calloc)(size_t blocks, size_t size);
    void (*mem_free)(void *block);
};

ds_status_e cc_rbuf_new(ds_rbuf_s **rbuf)
{
    ds_rbuf_cfg_s conf;
    cc_rbuf_conf_init(&conf);
    return cc_rbuf_conf_new(&conf, rbuf);
}

ds_status_e cc_rbuf_new_with_capacity(size_t capacity, ds_rbuf_s **rbuf)
{
    ds_rbuf_cfg_s conf;

    cc_rbuf_conf_init(&conf);
    /* 对外保持配置结构不透明，同时支持调用者指定缓冲容量。 */
    conf.capacity = capacity;
    return cc_rbuf_conf_new(&conf, rbuf);
}

ds_status_e cc_rbuf_conf_new(ds_rbuf_cfg_s *rconf, ds_rbuf_s **rbuf)
{
    if ((rconf == NULL) || (rbuf == NULL) || (rconf->capacity == 0U))
    {
        return DS_ERR_INVALID_CAPACITY;
    }

    ds_rbuf_s *ringbuf = rconf->mem_calloc(1, sizeof(ds_rbuf_s));

    if (!ringbuf)
    {
        return DS_ERR_ALLOC;
    }

    if (!(ringbuf->buf = rconf->mem_calloc(rconf->capacity, sizeof(QWORD))))
    {
        rconf->mem_free(ringbuf);
        return DS_ERR_ALLOC;
    }

    ringbuf->mem_alloc  = rconf->mem_alloc;
    ringbuf->mem_calloc = rconf->mem_calloc;
    ringbuf->mem_free   = rconf->mem_free;
    ringbuf->capacity   = rconf->capacity;
    ringbuf->size       = 0;
    ringbuf->head       = 0;
    ringbuf->tail       = 0;

    *rbuf = ringbuf;
    return DS_OK;
}

void cc_rbuf_conf_init(ds_rbuf_cfg_s *rconf)
{
    rconf->capacity   = DEFAULT_CC_RBUF_CAPACITY;
    rconf->mem_alloc  = malloc;
    rconf->mem_calloc = calloc;
    rconf->mem_free   = free;
}

void cc_rbuf_destroy(ds_rbuf_s *rbuf)
{
    rbuf->mem_free(rbuf->buf);
    rbuf->mem_free(rbuf);
}

BOOL cc_rbuf_is_empty(ds_rbuf_s *rbuf)
{
    return (rbuf->size == 0);
}

size_t cc_rbuf_size(ds_rbuf_s *rbuf)
{
    return rbuf->size;
}

size_t cc_rbuf_capacity(ds_rbuf_s *rbuf)
{
    return rbuf->capacity;
}

void cc_rbuf_enqueue(ds_rbuf_s *rbuf, QWORD item)
{
    if (rbuf->size == rbuf->capacity)
    {
        /* 满缓冲覆盖最旧元素，tail 始终指向下一次出队的位置。 */
        rbuf->tail = (rbuf->tail + 1) % rbuf->capacity;
    }
    else
    {
        ++rbuf->size;
    }

    rbuf->buf[rbuf->head] = item;

    rbuf->head = (rbuf->head + 1) % rbuf->capacity;
}

ds_status_e cc_rbuf_dequeue(ds_rbuf_s *rbuf, QWORD *out)
{
    if (cc_rbuf_is_empty(rbuf))
    {
        return DS_ERR_OUT_OF_RANGE;
    }

    *out = rbuf->buf[rbuf->tail];

    rbuf->tail = (rbuf->tail + 1) % rbuf->capacity;
    --rbuf->size;
    return DS_OK;
}

QWORD cc_rbuf_peek(ds_rbuf_s *rbuf, int index)
{
    return rbuf->buf[index];
}

size_t cc_rbuf_struct_size()
{
    return sizeof(ds_rbuf_s);
}
