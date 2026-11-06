/**
 * @Author: S. Sharma <silentcat>
 * @Date:   2019-03-07T10:32:52-06:00
 * @Email:  silentcat@protonmail.com
 * @Last modified by:   silentcat
 * @Last modified time: 2019-03-09T15:14:36-06:00
 */

#ifndef COLLECTIONS_C_CC_RBUF_H
#define COLLECTIONS_C_CC_RBUF_H

#ifdef __cplusplus
extern "C"
{
#endif

#include "config.h"
#include "ssd_types.h" /* BYTE/WORD/DWORD/QWORD */

#include <stdint.h>

#define DEFAULT_CC_RBUF_CAPACITY 10

    typedef struct DS_RING_BUFFER_CONFIG_S ds_rbuf_cfg_s;
    typedef struct DS_RING_BUFFER_S        ds_rbuf_s;

    ds_status_e cc_rbuf_new(ds_rbuf_s **rbuf);
    ds_status_e cc_rbuf_new_with_capacity(size_t capacity, ds_rbuf_s **rbuf);
    void        cc_rbuf_conf_init(ds_rbuf_cfg_s *rconf);
    ds_status_e cc_rbuf_conf_new(ds_rbuf_cfg_s *rconf, ds_rbuf_s **rbuf);
    size_t      cc_rbuf_struct_size();

    void        cc_rbuf_enqueue(ds_rbuf_s *rbuf, QWORD item);
    ds_status_e cc_rbuf_dequeue(ds_rbuf_s *rbuf, QWORD *out);
    BOOL        cc_rbuf_is_empty(ds_rbuf_s *rbuf);
    size_t      cc_rbuf_size(ds_rbuf_s *rbuf);
    size_t      cc_rbuf_capacity(ds_rbuf_s *rbuf);
    void        cc_rbuf_destroy(ds_rbuf_s *rbuf);
    QWORD       cc_rbuf_peek(ds_rbuf_s *rbuf, int index);

#ifdef __cplusplus
}
#endif

#endif
