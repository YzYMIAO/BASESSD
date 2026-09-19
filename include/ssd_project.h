#ifndef SSD_PROJECT_H
#define SSD_PROJECT_H

/* 工程公共入口：业务模块只需包含本文件即可获得基础类型、属性和诊断接口。 */
#include "common/common.h"
#include "common/ssd_assert.h"
#include "common/ssd_log.h"
#include "ssd_compiler.h"

/* 工程代码等级的语义名称，便于模块代码表达放置意图。 */
#define SSD_CODE_CRITICAL SSD_CODE_HIGH
#define SSD_CODE_HOT      SSD_CODE_HIGH
#define SSD_CODE_NORMAL   SSD_CODE_FAST
#define SSD_CODE_COLD     SSD_CODE_SLOW

/* HyperRAM 代码搬运由平台启动流程实现；未初始化前禁止调用 SSD_CODE_SLOW 函数。 */
static inline unsigned int ssd_hyperram_code_size(void)
{
    return (unsigned int)(__ssd_code_slow_end - __ssd_code_slow_start);
}

#endif /* SSD_PROJECT_H */
