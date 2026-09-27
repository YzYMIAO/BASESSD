#include "onfi_1_0.h"

#include "nand_config.h"

#include <stddef.h>

#define NAND_TIME_TWB_US   (1U)
#define NAND_TIME_TRST_US  (500U)
#define NAND_TIME_TPROG_US (700U)    /* tPROG typ=200µs, 3.5× 余量 */
#define NAND_TIME_TBERS_US (5000U)   /* tBERS typ=700µs, ~7× 余量 */
#define NAND_TIME_TR_US    (38U)     /* tR typ=25µs, 1.5× 余量 */
#define NAND_TIME_TWH_US   (1U)

static const nand_uop_t g_nand_normal_read_page_uops[] =
    {
        {NAND_UOP_COMMAND, NAND_CMD_READ_STATUS},
{NAND_UOP_DELAY_US, NAND_TIME_TWH_US},
{NAND_UOP_DATA_OUT_STATUS, 0U},
        {NAND_UOP_JUMP_IF_NOT_READY, 3U},
        {    NAND_UOP_COMMAND,   NAND_CMD_READ_PAGE_SETUP},
        {   NAND_UOP_COLUMN_0,                         0U},
        {   NAND_UOP_COLUMN_1,                         0U},
        {      NAND_UOP_ROW_0,                         0U},
        {      NAND_UOP_ROW_1,                         0U},
        {      NAND_UOP_ROW_2,                         0U},
        {    NAND_UOP_COMMAND, NAND_CMD_READ_PAGE_CONFIRM},
        { NAND_UOP_DELAY_US, NAND_TIME_TWB_US},
        { NAND_UOP_WAIT_READY,                         0U},
        {   NAND_UOP_DATA_OUT,                         0U},
        {NAND_UOP_COMMAND, NAND_CMD_READ_STATUS},
{NAND_UOP_DELAY_US, NAND_TIME_TWH_US},
{NAND_UOP_DATA_OUT_STATUS, 0U},
        {NAND_UOP_JUMP_IF_NOT_READY, 3U},
        { NAND_UOP_CHECK_FAIL,                         0U},
        {        NAND_UOP_END,                         0U},
};

static const nand_uop_t g_nand_normal_read_page_without_data_out_uops[] =
    {
        {NAND_UOP_COMMAND, NAND_CMD_READ_STATUS},
{NAND_UOP_DELAY_US, NAND_TIME_TWH_US},
{NAND_UOP_DATA_OUT_STATUS, 0U},
        {NAND_UOP_JUMP_IF_NOT_READY, 3U},
        {    NAND_UOP_COMMAND,   NAND_CMD_READ_PAGE_SETUP},
        {   NAND_UOP_COLUMN_0,                         0U},
        {   NAND_UOP_COLUMN_1,                         0U},
        {      NAND_UOP_ROW_0,                         0U},
        {      NAND_UOP_ROW_1,                         0U},
        {      NAND_UOP_ROW_2,                         0U},
        {    NAND_UOP_COMMAND, NAND_CMD_READ_PAGE_CONFIRM},
        { NAND_UOP_DELAY_US, NAND_TIME_TWB_US},
        { NAND_UOP_WAIT_READY,                         0U},
        {NAND_UOP_COMMAND, NAND_CMD_READ_STATUS},
{NAND_UOP_DELAY_US, NAND_TIME_TWH_US},
{NAND_UOP_DATA_OUT_STATUS, 0U},
        {NAND_UOP_JUMP_IF_NOT_READY, 3U},
        { NAND_UOP_CHECK_FAIL,                         0U},
        {        NAND_UOP_END,                         0U},
};

/* CACHE_START：专用于 Cache Read 管线初始化，语义独立于 WITHOUT_DATA_OUT */
static const nand_uop_t g_nand_cache_start_uops[] =
    {
        {NAND_UOP_COMMAND, NAND_CMD_READ_STATUS},
{NAND_UOP_DELAY_US, NAND_TIME_TWH_US},
{NAND_UOP_DATA_OUT_STATUS, 0U},
        {NAND_UOP_JUMP_IF_NOT_READY, 3U},
        {    NAND_UOP_COMMAND,   NAND_CMD_READ_PAGE_SETUP},
        {   NAND_UOP_COLUMN_0,                         0U},
        {   NAND_UOP_COLUMN_1,                         0U},
        {      NAND_UOP_ROW_0,                         0U},
        {      NAND_UOP_ROW_1,                         0U},
        {      NAND_UOP_ROW_2,                         0U},
        {    NAND_UOP_COMMAND, NAND_CMD_READ_PAGE_CONFIRM},
        { NAND_UOP_DELAY_US, NAND_TIME_TWB_US},
        { NAND_UOP_WAIT_READY,                         0U},
        {NAND_UOP_COMMAND, NAND_CMD_READ_STATUS},
{NAND_UOP_DELAY_US, NAND_TIME_TWH_US},
{NAND_UOP_DATA_OUT_STATUS, 0U},
        {NAND_UOP_JUMP_IF_NOT_READY, 3U},
        { NAND_UOP_CHECK_FAIL,                         0U},
        {        NAND_UOP_END,                         0U},
};

static const nand_uop_t g_nand_normal_program_page_uops[] =
    {
        {NAND_UOP_COMMAND, NAND_CMD_READ_STATUS},
{NAND_UOP_DELAY_US, NAND_TIME_TWH_US},
{NAND_UOP_DATA_OUT_STATUS, 0U},
        {NAND_UOP_JUMP_IF_NOT_READY, 3U},
        {    NAND_UOP_COMMAND,   NAND_CMD_PROGRAM_PAGE_SETUP},
        {   NAND_UOP_COLUMN_0,                            0U},
        {   NAND_UOP_COLUMN_1,                            0U},
        {      NAND_UOP_ROW_0,                            0U},
        {      NAND_UOP_ROW_1,                            0U},
        {      NAND_UOP_ROW_2,                            0U},
        {    NAND_UOP_DATA_IN,                            0U},
        {    NAND_UOP_COMMAND, NAND_CMD_PROGRAM_PAGE_CONFIRM},
        { NAND_UOP_DELAY_US, NAND_TIME_TWB_US},
        { NAND_UOP_WAIT_READY,                            0U},
        {NAND_UOP_COMMAND, NAND_CMD_READ_STATUS},
{NAND_UOP_DELAY_US, NAND_TIME_TWH_US},
{NAND_UOP_DATA_OUT_STATUS, 0U},
        {NAND_UOP_JUMP_IF_NOT_READY, 3U},
        { NAND_UOP_CHECK_FAIL,                            0U},
        {        NAND_UOP_END,                            0U},
};

static const nand_uop_t g_nand_erase_block_uops[] =
    {
        {NAND_UOP_COMMAND, NAND_CMD_READ_STATUS},
{NAND_UOP_DELAY_US, NAND_TIME_TWH_US},
{NAND_UOP_DATA_OUT_STATUS, 0U},
        {NAND_UOP_JUMP_IF_NOT_READY, 3U},
        {    NAND_UOP_COMMAND,   NAND_CMD_BLOCK_ERASE_SETUP},
        {      NAND_UOP_ROW_0,                           0U},
        {      NAND_UOP_ROW_1,                           0U},
        {      NAND_UOP_ROW_2,                           0U},
        {    NAND_UOP_COMMAND, NAND_CMD_BLOCK_ERASE_CONFIRM},
        { NAND_UOP_DELAY_US, NAND_TIME_TWB_US},
        { NAND_UOP_WAIT_READY,                           0U},
        {NAND_UOP_COMMAND, NAND_CMD_READ_STATUS},
{NAND_UOP_DELAY_US, NAND_TIME_TWH_US},
{NAND_UOP_DATA_OUT_STATUS, 0U},
        {NAND_UOP_JUMP_IF_NOT_READY, 3U},
        { NAND_UOP_CHECK_FAIL,                           0U},
        {        NAND_UOP_END,                           0U},
};

static const nand_uop_t g_nand_reset_uops[] = {
    {     NAND_UOP_WAIT_READY, 0U},
    {    NAND_UOP_COMMAND, NAND_ONFI_CMD_RESET},
    {     NAND_UOP_WAIT_READY, 0U},
    {NAND_UOP_COMMAND, NAND_CMD_READ_STATUS},
{NAND_UOP_DELAY_US, NAND_TIME_TWH_US},
{NAND_UOP_DATA_OUT_STATUS, 0U},
    {NAND_UOP_JUMP_IF_NOT_READY, 3U},
    {     NAND_UOP_CHECK_FAIL, 0U},
    {            NAND_UOP_END, 0U}
};
/* READ_PARAMETER_PAGE / READ_UNIQUE_ID / GET_FEATURES 共用相同的 UOP 序列，
 * 但命令字节不同。拆分后各序列自包含，不再查 operation_map。 */
static const nand_uop_t g_nand_read_parameter_page_uops[] = {
    {NAND_UOP_COMMAND, NAND_CMD_READ_STATUS},
{NAND_UOP_DELAY_US, NAND_TIME_TWH_US},
{NAND_UOP_DATA_OUT_STATUS, 0U},
    {NAND_UOP_JUMP_IF_NOT_READY, 3U},
    {    NAND_UOP_COMMAND, NAND_CMD_READ_PARAMETER_PAGE}, /* ECh */
    {       NAND_UOP_COLUMN_0, 0U},
    {    NAND_UOP_WAIT_READY, 0U},
    {       NAND_UOP_DATA_OUT, 0U},
    {NAND_UOP_COMMAND, NAND_CMD_READ_STATUS},
{NAND_UOP_DELAY_US, NAND_TIME_TWH_US},
{NAND_UOP_DATA_OUT_STATUS, 0U},
    {NAND_UOP_JUMP_IF_NOT_READY, 3U},
    {            NAND_UOP_END, 0U}
};
static const nand_uop_t g_nand_read_unique_id_uops[] = {
    {NAND_UOP_COMMAND, NAND_CMD_READ_STATUS},
{NAND_UOP_DELAY_US, NAND_TIME_TWH_US},
{NAND_UOP_DATA_OUT_STATUS, 0U},
    {NAND_UOP_JUMP_IF_NOT_READY, 3U},
    {    NAND_UOP_COMMAND, NAND_CMD_READ_UNIQUE_ID},       /* EDh */
    {       NAND_UOP_COLUMN_0, 0U},
    {    NAND_UOP_WAIT_READY, 0U},
    {       NAND_UOP_DATA_OUT, 0U},
    {NAND_UOP_COMMAND, NAND_CMD_READ_STATUS},
{NAND_UOP_DELAY_US, NAND_TIME_TWH_US},
{NAND_UOP_DATA_OUT_STATUS, 0U},
    {NAND_UOP_JUMP_IF_NOT_READY, 3U},
    {            NAND_UOP_END, 0U}
};
static const nand_uop_t g_nand_get_features_uops[] = {
    {NAND_UOP_COMMAND, NAND_CMD_READ_STATUS},
{NAND_UOP_DELAY_US, NAND_TIME_TWH_US},
{NAND_UOP_DATA_OUT_STATUS, 0U},
    {NAND_UOP_JUMP_IF_NOT_READY, 3U},
    {    NAND_UOP_COMMAND, NAND_CMD_GET_FEATURES},         /* EEh */
    {       NAND_UOP_COLUMN_0, 0U},
    {    NAND_UOP_WAIT_READY, 0U},
    {       NAND_UOP_DATA_OUT, 0U},
    {NAND_UOP_COMMAND, NAND_CMD_READ_STATUS},
{NAND_UOP_DELAY_US, NAND_TIME_TWH_US},
{NAND_UOP_DATA_OUT_STATUS, 0U},
    {NAND_UOP_JUMP_IF_NOT_READY, 3U},
    {            NAND_UOP_END, 0U}
};
/*
 * Read ID is the board bring-up command. Keep it identical to the vendor reference:
 * wait for R/B#, then 90h -> 00h -> data. Do not prepend 70h status polling here,
 * because an all-identical result is otherwise indistinguishable from stale status data.
 */
static const nand_uop_t g_nand_read_id_uops[] = {
    {     NAND_UOP_WAIT_READY, 0U},
    {    NAND_UOP_COMMAND, NAND_CMD_READ_ID},  /* 90h */
    {       NAND_UOP_COLUMN_0, 0U},
    {       NAND_UOP_DATA_OUT, 0U},
    {            NAND_UOP_END, 0U}
};
static const nand_uop_t g_nand_set_features_uops[] = {
    {NAND_UOP_COMMAND, NAND_CMD_READ_STATUS},
{NAND_UOP_DELAY_US, NAND_TIME_TWH_US},
{NAND_UOP_DATA_OUT_STATUS, 0U},
    {NAND_UOP_JUMP_IF_NOT_READY, 3U},
    {    NAND_UOP_COMMAND, NAND_CMD_SET_FEATURES},  /* EFh */
    {       NAND_UOP_COLUMN_0, 0U},
    {        NAND_UOP_DATA_IN, 0U},
    {     NAND_UOP_WAIT_READY, 0U},
    {NAND_UOP_COMMAND, NAND_CMD_READ_STATUS},
{NAND_UOP_DELAY_US, NAND_TIME_TWH_US},
{NAND_UOP_DATA_OUT_STATUS, 0U},
    {NAND_UOP_JUMP_IF_NOT_READY, 3U},
    {     NAND_UOP_CHECK_FAIL, 0U},
    {            NAND_UOP_END, 0U}
};
static const nand_uop_t g_nand_read_status_uops[] = {
    {  NAND_UOP_COMMAND, NAND_CMD_READ_STATUS},  /* 70h */
    {NAND_UOP_DATA_OUT_STATUS, 0U},
    {            NAND_UOP_END, 0U}
};
static const nand_uop_t g_nand_read_status_enhanced_uops[] = {
    {    NAND_UOP_COMMAND, NAND_CMD_READ_STATUS_ENHANCED},  /* 78h: 指定地址读任意 LUN/Plane 状态 */
    {          NAND_UOP_ROW_0, 0U},
    {          NAND_UOP_ROW_1, 0U},
    {          NAND_UOP_ROW_2, 0U},
    { NAND_UOP_DELAY_US, NAND_TIME_TWH_US},
{NAND_UOP_DATA_OUT_STATUS, 0U},
    {            NAND_UOP_END, 0U}
};
static const nand_uop_t g_nand_random_read_uops[] = {
    {NAND_UOP_COMMAND, NAND_CMD_READ_STATUS},
{NAND_UOP_DELAY_US, NAND_TIME_TWH_US},
{NAND_UOP_DATA_OUT_STATUS, 0U},
    {NAND_UOP_JUMP_IF_NOT_READY, 3U},
    {  NAND_UOP_COMMAND, NAND_CMD_RANDOM_DATA_READ},           /* 05h */
    {         NAND_UOP_COLUMN_0, 0U},
    {         NAND_UOP_COLUMN_1, 0U},
    {  NAND_UOP_COMMAND, NAND_CMD_RANDOM_DATA_READ_END},       /* E0h */
    {       NAND_UOP_WAIT_READY, 0U},
    {         NAND_UOP_DATA_OUT, 0U},
    {NAND_UOP_COMMAND, NAND_CMD_READ_STATUS},
{NAND_UOP_DELAY_US, NAND_TIME_TWH_US},
{NAND_UOP_DATA_OUT_STATUS, 0U},
    {NAND_UOP_JUMP_IF_NOT_READY, 3U},
    {              NAND_UOP_END, 0U}
};
static const nand_uop_t g_nand_program_move_uops[] = {
    {NAND_UOP_COMMAND, NAND_CMD_READ_STATUS},
{NAND_UOP_DELAY_US, NAND_TIME_TWH_US},
{NAND_UOP_DATA_OUT_STATUS, 0U},
    {NAND_UOP_JUMP_IF_NOT_READY, 3U},
    {  NAND_UOP_COMMAND, NAND_CMD_RANDOM_DATA_INPUT},           /* 85h */
    {         NAND_UOP_COLUMN_0, 0U},
    {         NAND_UOP_COLUMN_1, 0U},
    {            NAND_UOP_ROW_0, 0U},
    {            NAND_UOP_ROW_1, 0U},
    {            NAND_UOP_ROW_2, 0U},
    {          NAND_UOP_DATA_IN, 0U},
    {  NAND_UOP_COMMAND, NAND_CMD_PROGRAM_PAGE_CONFIRM},        /* 10h */
    { NAND_UOP_DELAY_US, NAND_TIME_TWB_US},
    {       NAND_UOP_WAIT_READY, 0U},
    {NAND_UOP_COMMAND, NAND_CMD_READ_STATUS},
{NAND_UOP_DELAY_US, NAND_TIME_TWH_US},
{NAND_UOP_DATA_OUT_STATUS, 0U},
    {NAND_UOP_JUMP_IF_NOT_READY, 3U},
    {       NAND_UOP_CHECK_FAIL, 0U},
    {              NAND_UOP_END, 0U}
};
/* READ_PAGE_CACHE_SEQ(31h) 与 READ_PAGE_CACHE_LAST(3Fh) UOP 序列相同但命令值不同 */
static const nand_uop_t g_nand_cache_read_seq_uops[] = {
    {NAND_UOP_COMMAND, NAND_CMD_READ_STATUS},
{NAND_UOP_DELAY_US, NAND_TIME_TWH_US},
{NAND_UOP_DATA_OUT_STATUS, 0U},
    {NAND_UOP_JUMP_IF_NOT_READY, 3U},
    {    NAND_UOP_COMMAND, NAND_CMD_READ_PAGE_CACHE_SEQ},  /* 31h */
    {     NAND_UOP_WAIT_READY, 0U},
    {       NAND_UOP_DATA_OUT, 0U},
    {NAND_UOP_COMMAND, NAND_CMD_READ_STATUS},
{NAND_UOP_DELAY_US, NAND_TIME_TWH_US},
{NAND_UOP_DATA_OUT_STATUS, 0U},
    {NAND_UOP_JUMP_IF_NOT_READY, 3U},
    {            NAND_UOP_END, 0U}
};
static const nand_uop_t g_nand_cache_read_last_uops[] = {
    {NAND_UOP_COMMAND, NAND_CMD_READ_STATUS},
{NAND_UOP_DELAY_US, NAND_TIME_TWH_US},
{NAND_UOP_DATA_OUT_STATUS, 0U},
    {NAND_UOP_JUMP_IF_NOT_READY, 3U},
    {    NAND_UOP_COMMAND, NAND_CMD_READ_PAGE_CACHE_LAST},  /* 3Fh */
    {     NAND_UOP_WAIT_READY, 0U},
    {       NAND_UOP_DATA_OUT, 0U},
    {NAND_UOP_COMMAND, NAND_CMD_READ_STATUS},
{NAND_UOP_DELAY_US, NAND_TIME_TWH_US},
{NAND_UOP_DATA_OUT_STATUS, 0U},
    {NAND_UOP_JUMP_IF_NOT_READY, 3U},
    {            NAND_UOP_END, 0U}
};
static const nand_uop_t g_nand_cache_read_random_uops[] = {
    {NAND_UOP_COMMAND, NAND_CMD_READ_STATUS},
{NAND_UOP_DELAY_US, NAND_TIME_TWH_US},
{NAND_UOP_DATA_OUT_STATUS, 0U},
    {NAND_UOP_JUMP_IF_NOT_READY, 3U},
    {  NAND_UOP_COMMAND, NAND_CMD_READ_PAGE_SETUP},             /* 00h */
    {         NAND_UOP_COLUMN_0, 0U},
    {         NAND_UOP_COLUMN_1, 0U},
    {            NAND_UOP_ROW_0, 0U},
    {            NAND_UOP_ROW_1, 0U},
    {            NAND_UOP_ROW_2, 0U},
    {  NAND_UOP_COMMAND, NAND_CMD_READ_PAGE_CACHE_SEQ},         /* 31h */
    {       NAND_UOP_WAIT_READY, 0U},
    {         NAND_UOP_DATA_OUT, 0U},
    {NAND_UOP_COMMAND, NAND_CMD_READ_STATUS},
{NAND_UOP_DELAY_US, NAND_TIME_TWH_US},
{NAND_UOP_DATA_OUT_STATUS, 0U},
    {NAND_UOP_JUMP_IF_NOT_READY, 3U},
    {              NAND_UOP_END, 0U}
};
static const nand_uop_t g_nand_program_cache_uops[] = {
    {NAND_UOP_COMMAND, NAND_CMD_READ_STATUS},
{NAND_UOP_DELAY_US, NAND_TIME_TWH_US},
{NAND_UOP_DATA_OUT_STATUS, 0U},
    {NAND_UOP_JUMP_IF_NOT_READY, 3U},
    {  NAND_UOP_COMMAND, NAND_CMD_PROGRAM_PAGE_SETUP},          /* 80h */
    {         NAND_UOP_COLUMN_0, 0U},
    {         NAND_UOP_COLUMN_1, 0U},
    {            NAND_UOP_ROW_0, 0U},
    {            NAND_UOP_ROW_1, 0U},
    {            NAND_UOP_ROW_2, 0U},
    {          NAND_UOP_DATA_IN, 0U},
    {  NAND_UOP_COMMAND, NAND_CMD_PROGRAM_PAGE_CACHE},          /* 15h */
    { NAND_UOP_DELAY_US, NAND_TIME_TWB_US},
    {       NAND_UOP_WAIT_READY, 0U},
    {NAND_UOP_COMMAND, NAND_CMD_READ_STATUS},
{NAND_UOP_DELAY_US, NAND_TIME_TWH_US},
{NAND_UOP_DATA_OUT_STATUS, 0U},
    {NAND_UOP_JUMP_IF_NOT_READY, 3U},
    {       NAND_UOP_CHECK_FAIL, 0U},
    {              NAND_UOP_END, 0U}
};
static const nand_uop_t g_nand_internal_read_uops[] = {
    {NAND_UOP_COMMAND, NAND_CMD_READ_STATUS},
{NAND_UOP_DELAY_US, NAND_TIME_TWH_US},
{NAND_UOP_DATA_OUT_STATUS, 0U},
    {NAND_UOP_JUMP_IF_NOT_READY, 3U},
    {  NAND_UOP_COMMAND, NAND_CMD_READ_PAGE_SETUP},             /* 00h */
    {         NAND_UOP_COLUMN_0, 0U},
    {         NAND_UOP_COLUMN_1, 0U},
    {            NAND_UOP_ROW_0, 0U},
    {            NAND_UOP_ROW_1, 0U},
    {            NAND_UOP_ROW_2, 0U},
    {  NAND_UOP_COMMAND, NAND_CMD_INTERNAL_DATA_MOVE},          /* 35h */
    { NAND_UOP_DELAY_US, NAND_TIME_TWB_US},
    {       NAND_UOP_WAIT_READY, 0U},
    {NAND_UOP_COMMAND, NAND_CMD_READ_STATUS},
{NAND_UOP_DELAY_US, NAND_TIME_TWH_US},
{NAND_UOP_DATA_OUT_STATUS, 0U},
    {NAND_UOP_JUMP_IF_NOT_READY, 3U},
    {       NAND_UOP_CHECK_FAIL, 0U},
    {              NAND_UOP_END, 0U}
};
static const nand_uop_t g_nand_internal_program_uops[] = {
    {NAND_UOP_COMMAND, NAND_CMD_READ_STATUS},
{NAND_UOP_DELAY_US, NAND_TIME_TWH_US},
{NAND_UOP_DATA_OUT_STATUS, 0U},
    {NAND_UOP_JUMP_IF_NOT_READY, 3U},
    {  NAND_UOP_COMMAND, NAND_CMD_RANDOM_DATA_INPUT},           /* 85h */
    {         NAND_UOP_COLUMN_0, 0U},
    {         NAND_UOP_COLUMN_1, 0U},
    {            NAND_UOP_ROW_0, 0U},
    {            NAND_UOP_ROW_1, 0U},
    {            NAND_UOP_ROW_2, 0U},
    {  NAND_UOP_COMMAND, NAND_CMD_PROGRAM_PAGE_CONFIRM},        /* 10h */
    { NAND_UOP_DELAY_US, NAND_TIME_TWB_US},
    {       NAND_UOP_WAIT_READY, 0U},
    {NAND_UOP_COMMAND, NAND_CMD_READ_STATUS},
{NAND_UOP_DELAY_US, NAND_TIME_TWH_US},
{NAND_UOP_DATA_OUT_STATUS, 0U},
    {NAND_UOP_JUMP_IF_NOT_READY, 3U},
    {       NAND_UOP_CHECK_FAIL, 0U},
    {              NAND_UOP_END, 0U}
};
/* BLOCK_UNLOCK_LOW(23h) 与 BLOCK_UNLOCK_HIGH(24h) 序列相同但命令值不同 */
static const nand_uop_t g_nand_block_unlock_low_uops[] = {
    {NAND_UOP_COMMAND, NAND_CMD_READ_STATUS},
{NAND_UOP_DELAY_US, NAND_TIME_TWH_US},
{NAND_UOP_DATA_OUT_STATUS, 0U},
    {NAND_UOP_JUMP_IF_NOT_READY, 3U},
    {    NAND_UOP_COMMAND, NAND_CMD_BLOCK_UNLOCK_LOW},  /* 23h */
    {          NAND_UOP_ROW_0, 0U},
    {          NAND_UOP_ROW_1, 0U},
    {          NAND_UOP_ROW_2, 0U},
    {NAND_UOP_COMMAND, NAND_CMD_READ_STATUS},
{NAND_UOP_DELAY_US, NAND_TIME_TWH_US},
{NAND_UOP_DATA_OUT_STATUS, 0U},
    {NAND_UOP_JUMP_IF_NOT_READY, 3U},
    {            NAND_UOP_END, 0U}
};
static const nand_uop_t g_nand_block_unlock_high_uops[] = {
    {NAND_UOP_COMMAND, NAND_CMD_READ_STATUS},
{NAND_UOP_DELAY_US, NAND_TIME_TWH_US},
{NAND_UOP_DATA_OUT_STATUS, 0U},
    {NAND_UOP_JUMP_IF_NOT_READY, 3U},
    {    NAND_UOP_COMMAND, NAND_CMD_BLOCK_UNLOCK_HIGH},  /* 24h */
    {          NAND_UOP_ROW_0, 0U},
    {          NAND_UOP_ROW_1, 0U},
    {          NAND_UOP_ROW_2, 0U},
    {NAND_UOP_COMMAND, NAND_CMD_READ_STATUS},
{NAND_UOP_DELAY_US, NAND_TIME_TWH_US},
{NAND_UOP_DATA_OUT_STATUS, 0U},
    {NAND_UOP_JUMP_IF_NOT_READY, 3U},
    {            NAND_UOP_END, 0U}
};
/* BLOCK_LOCK(2Ah) 与 BLOCK_LOCK_TIGHT(2Ch) 序列相同但命令值不同 */
static const nand_uop_t g_nand_block_lock_uops[] = {
    {NAND_UOP_COMMAND, NAND_CMD_READ_STATUS},
{NAND_UOP_DELAY_US, NAND_TIME_TWH_US},
{NAND_UOP_DATA_OUT_STATUS, 0U},
    {NAND_UOP_JUMP_IF_NOT_READY, 3U},
    {    NAND_UOP_COMMAND, NAND_CMD_BLOCK_LOCK},  /* 2Ah */
    {NAND_UOP_COMMAND, NAND_CMD_READ_STATUS},
{NAND_UOP_DELAY_US, NAND_TIME_TWH_US},
{NAND_UOP_DATA_OUT_STATUS, 0U},
    {NAND_UOP_JUMP_IF_NOT_READY, 3U},
    {            NAND_UOP_END, 0U}
};
static const nand_uop_t g_nand_block_lock_tight_uops[] = {
    {NAND_UOP_COMMAND, NAND_CMD_READ_STATUS},
{NAND_UOP_DELAY_US, NAND_TIME_TWH_US},
{NAND_UOP_DATA_OUT_STATUS, 0U},
    {NAND_UOP_JUMP_IF_NOT_READY, 3U},
    {    NAND_UOP_COMMAND, NAND_CMD_BLOCK_LOCK_TIGHT},  /* 2Ch */
    {NAND_UOP_COMMAND, NAND_CMD_READ_STATUS},
{NAND_UOP_DELAY_US, NAND_TIME_TWH_US},
{NAND_UOP_DATA_OUT_STATUS, 0U},
    {NAND_UOP_JUMP_IF_NOT_READY, 3U},
    {            NAND_UOP_END, 0U}
};
static const nand_uop_t g_nand_block_lock_status_uops[] = {
    {NAND_UOP_COMMAND, NAND_CMD_READ_STATUS},
{NAND_UOP_DELAY_US, NAND_TIME_TWH_US},
{NAND_UOP_DATA_OUT_STATUS, 0U},
    {NAND_UOP_JUMP_IF_NOT_READY, 3U},
    {    NAND_UOP_COMMAND, NAND_CMD_BLOCK_LOCK_STATUS},  /* 7Ah */
    {          NAND_UOP_ROW_0, 0U},
    {          NAND_UOP_ROW_1, 0U},
    {          NAND_UOP_ROW_2, 0U},
    {NAND_UOP_DATA_OUT_STATUS, 0U},
    {            NAND_UOP_END, 0U}
};
static const nand_uop_t g_nand_otp_lock_uops[] = {
    {NAND_UOP_COMMAND, NAND_CMD_READ_STATUS},
{NAND_UOP_DELAY_US, NAND_TIME_TWH_US},
{NAND_UOP_DATA_OUT_STATUS, 0U},
    {NAND_UOP_JUMP_IF_NOT_READY, 3U},
    {  NAND_UOP_COMMAND, NAND_CMD_PROGRAM_PAGE_SETUP},  /* 80h */
    {         NAND_UOP_COLUMN_0, 0U},
    {         NAND_UOP_COLUMN_1, 0U},
    {            NAND_UOP_ROW_0, 0U},
    {            NAND_UOP_ROW_1, 0U},
    {            NAND_UOP_ROW_2, 0U},
    {  NAND_UOP_COMMAND, NAND_CMD_PROGRAM_PAGE_CONFIRM},  /* 10h */
    { NAND_UOP_DELAY_US, NAND_TIME_TWB_US},
    {       NAND_UOP_WAIT_READY, 0U},
    {NAND_UOP_COMMAND, NAND_CMD_READ_STATUS},
{NAND_UOP_DELAY_US, NAND_TIME_TWH_US},
{NAND_UOP_DATA_OUT_STATUS, 0U},
    {NAND_UOP_JUMP_IF_NOT_READY, 3U},
    {       NAND_UOP_CHECK_FAIL, 0U},
    {              NAND_UOP_END, 0U}
};

/* ======================================================================
 * Multi-Plane uop 序列
 * ====================================================================== */

static const nand_uop_t g_nand_multi_plane_read_with_data_out_uops[] =
    {
        {NAND_UOP_COMMAND, NAND_CMD_READ_STATUS},
{NAND_UOP_DELAY_US, NAND_TIME_TWH_US},
{NAND_UOP_DATA_OUT_STATUS, 0U},
        {NAND_UOP_JUMP_IF_NOT_READY, 3U},
 /* 第一组: 00h + 地址周期 (Plane 0) */
        {    NAND_UOP_COMMAND,   NAND_CMD_READ_PAGE_SETUP},
        {   NAND_UOP_COLUMN_0,                         0U},
        {   NAND_UOP_COLUMN_1,                         0U},
        {      NAND_UOP_ROW_0,                         0U},
        {      NAND_UOP_ROW_1,                         0U},
        {      NAND_UOP_ROW_2,                         0U},
 /* 第二组: 00h + 地址周期 (Plane 1) */
        {    NAND_UOP_COMMAND,   NAND_CMD_READ_PAGE_SETUP},
        {   NAND_UOP_COLUMN_0,                         0U},
        {   NAND_UOP_COLUMN_1,                         0U},
        {      NAND_UOP_ROW_0,                         1U},
        {      NAND_UOP_ROW_1,                         1U},
        {      NAND_UOP_ROW_2,                         1U},
 /* 30h + tR + 双页数据输出 */
        {    NAND_UOP_COMMAND, NAND_CMD_READ_PAGE_CONFIRM},           /* 30h */
        { NAND_UOP_WAIT_READY,                                  0U},  /* tR */
        {    NAND_UOP_COMMAND, NAND_CMD_RANDOM_DATA_READ_TWO_PLANE},  /* 06h: 选择 Plane 0 */
        {   NAND_UOP_COLUMN_0,                                  0U},
        {   NAND_UOP_COLUMN_1,                                  0U},
        {      NAND_UOP_ROW_0,                                  0U},  /* row[0] = plane 0 */
        {      NAND_UOP_ROW_1,                                  0U},
        {      NAND_UOP_ROW_2,                                  0U},
        {    NAND_UOP_COMMAND, NAND_CMD_RANDOM_DATA_READ_END},        /* E0h */
        {   NAND_UOP_DATA_OUT,                                  0U},  /* Plane 0 data: page_buffer[0..2111] */
        {    NAND_UOP_COMMAND, NAND_CMD_RANDOM_DATA_READ_TWO_PLANE},  /* 06h: 选择 Plane 1 */
        {   NAND_UOP_COLUMN_0,                                  0U},
        {   NAND_UOP_COLUMN_1,                                  0U},
        {      NAND_UOP_ROW_0,                                  1U},  /* row[1] = plane 1 */
        {      NAND_UOP_ROW_1,                                  1U},
        {      NAND_UOP_ROW_2,                                  1U},
        {    NAND_UOP_COMMAND, NAND_CMD_RANDOM_DATA_READ_END},        /* E0h */
        {   NAND_UOP_DATA_OUT,                            PAGE_SIZE},  /* Plane 1 data: page_buffer[2112..4223] */
        {NAND_UOP_COMMAND, NAND_CMD_READ_STATUS},
{NAND_UOP_DELAY_US, NAND_TIME_TWH_US},
{NAND_UOP_DATA_OUT_STATUS, 0U},
        {NAND_UOP_JUMP_IF_NOT_READY, 3U},
        { NAND_UOP_CHECK_FAIL,                         0U},
        {        NAND_UOP_END,                         0U},
};

static const nand_uop_t g_nand_multi_plane_read_without_data_out_uops[] =
    {
        {NAND_UOP_COMMAND, NAND_CMD_READ_STATUS},
{NAND_UOP_DELAY_US, NAND_TIME_TWH_US},
{NAND_UOP_DATA_OUT_STATUS, 0U},
        {NAND_UOP_JUMP_IF_NOT_READY, 3U},
 /* 第一组: 00h + 地址周期 (Plane 0) */
        {    NAND_UOP_COMMAND,   NAND_CMD_READ_PAGE_SETUP},
        {   NAND_UOP_COLUMN_0,                         0U},
        {   NAND_UOP_COLUMN_1,                         0U},
        {      NAND_UOP_ROW_0,                         0U},
        {      NAND_UOP_ROW_1,                         0U},
        {      NAND_UOP_ROW_2,                         0U},
 /* 第二组: 00h + 地址周期 (Plane 1) */
        {    NAND_UOP_COMMAND,   NAND_CMD_READ_PAGE_SETUP},
        {   NAND_UOP_COLUMN_0,                         0U},
        {   NAND_UOP_COLUMN_1,                         0U},
        {      NAND_UOP_ROW_0,                         1U},
        {      NAND_UOP_ROW_1,                         1U},
        {      NAND_UOP_ROW_2,                         1U},
 /* 30h + tR，无数据输出（数据留在页缓冲中） */
        {    NAND_UOP_COMMAND, NAND_CMD_READ_PAGE_CONFIRM},
        { NAND_UOP_DELAY_US, NAND_TIME_TWB_US},
        { NAND_UOP_WAIT_READY,                         0U},
        {NAND_UOP_COMMAND, NAND_CMD_READ_STATUS},
{NAND_UOP_DELAY_US, NAND_TIME_TWH_US},
{NAND_UOP_DATA_OUT_STATUS, 0U},
        {NAND_UOP_JUMP_IF_NOT_READY, 3U},
        { NAND_UOP_CHECK_FAIL,                         0U},
        {        NAND_UOP_END,                         0U},
};

static const nand_uop_t g_nand_multi_plane_program_uops[] =
    {
        {NAND_UOP_COMMAND, NAND_CMD_READ_STATUS},
{NAND_UOP_DELAY_US, NAND_TIME_TWH_US},
{NAND_UOP_DATA_OUT_STATUS, 0U},
        {NAND_UOP_JUMP_IF_NOT_READY, 3U},
 /* ① 80h + 地址 + Din (Plane 0) */
        {    NAND_UOP_COMMAND,         NAND_CMD_PROGRAM_PAGE_SETUP},
        {   NAND_UOP_COLUMN_0,                                  0U},
        {   NAND_UOP_COLUMN_1,                                  0U},
        {      NAND_UOP_ROW_0,                                  0U}, /* row[0]——plane 0 */
        {      NAND_UOP_ROW_1,                                  0U},
        {      NAND_UOP_ROW_2,                                  0U},
        {    NAND_UOP_DATA_IN,                                  0U}, /* Plane 0 全页: buffer[0..2111] */
  /* ③ 11h 切换 plane + 地址 + Din (Plane 1) */
        {    NAND_UOP_COMMAND, NAND_CMD_MULTI_PLANE_PROGRAM_SWITCH}, /* 11h */
        { NAND_UOP_DELAY_US,                                  1U}, /* tADL: 11h 切换 plane */
        {    NAND_UOP_COMMAND,         NAND_CMD_PROGRAM_PAGE_SETUP}, /* 80h: Plane 1 地址周期 */
        {   NAND_UOP_COLUMN_0,                                  0U},
        {   NAND_UOP_COLUMN_1,                                  0U},
        {      NAND_UOP_ROW_0,                                  1U}, /* row[1]——plane 1 */
        {      NAND_UOP_ROW_1,                                  1U},
        {      NAND_UOP_ROW_2,                                  1U},
        {    NAND_UOP_DATA_IN,                        PAGE_SIZE}, /* Plane 1 全页: buffer[2112..4223] */
  /* ⑥ 10h 确认 + tPROG */
        {    NAND_UOP_COMMAND,       NAND_CMD_PROGRAM_PAGE_CONFIRM},
        { NAND_UOP_DELAY_US, NAND_TIME_TWB_US},
        { NAND_UOP_WAIT_READY,                                  0U},
        {NAND_UOP_COMMAND, NAND_CMD_READ_STATUS},
{NAND_UOP_DELAY_US, NAND_TIME_TWH_US},
{NAND_UOP_DATA_OUT_STATUS, 0U},
        {NAND_UOP_JUMP_IF_NOT_READY, 3U},
        { NAND_UOP_CHECK_FAIL,                                  0U},
        {        NAND_UOP_END,                                  0U},
};

static const nand_uop_t g_nand_multi_plane_erase_uops[] =
    {
        {NAND_UOP_COMMAND, NAND_CMD_READ_STATUS},
{NAND_UOP_DELAY_US, NAND_TIME_TWH_US},
{NAND_UOP_DATA_OUT_STATUS, 0U},
        {NAND_UOP_JUMP_IF_NOT_READY, 3U},
 /* ① 60h + 行地址 (Plane 0，无列地址) */
        {    NAND_UOP_COMMAND,   NAND_CMD_BLOCK_ERASE_SETUP},
        {      NAND_UOP_ROW_0,                           0U}, /* row[0]——plane 0 */
        {      NAND_UOP_ROW_1,                           0U},
        {      NAND_UOP_ROW_2,                           0U},
 /* ③ 第二次 60h + 行地址 (Plane 1) */
        {    NAND_UOP_COMMAND,   NAND_CMD_BLOCK_ERASE_SETUP},
        {      NAND_UOP_ROW_0,                           1U}, /* row[1]——plane 1 */
        {      NAND_UOP_ROW_1,                           1U},
        {      NAND_UOP_ROW_2,                           1U},
 /* ⑤ D0h 确认 + tBERS */
        {    NAND_UOP_COMMAND, NAND_CMD_BLOCK_ERASE_CONFIRM},
        { NAND_UOP_DELAY_US, NAND_TIME_TWB_US},
        { NAND_UOP_WAIT_READY,                           0U},
        {NAND_UOP_COMMAND, NAND_CMD_READ_STATUS},
{NAND_UOP_DELAY_US, NAND_TIME_TWH_US},
{NAND_UOP_DATA_OUT_STATUS, 0U},
        {NAND_UOP_JUMP_IF_NOT_READY, 3U},
        { NAND_UOP_CHECK_FAIL,                           0U},
        {        NAND_UOP_END,                           0U},
};

const nand_operation_map_t g_nand_operation_map[OP_COUNT] =
    {
        [OP_RESET]                                  = {                                 .operation = OP_RESET,                          .name = "RESET",           .command_1 = NAND_ONFI_CMD_RESET,                            .command_2 = 0U,  .address_cycles = 0U, .data_input_cycles = 0U, .valid_while_selected_lun_busy = 0U, .valid_while_other_lun_busy = 1U, .has_data = 0U,                   .data_length = 0U,                             .uops = g_nand_reset_uops, .timing = {NAND_TIME_TWB_US, NAND_TIME_TRST_US, NAND_TIME_TWH_US, NAND_TIME_TRST_US}},
        [OP_READ_ID]                                = {                               .operation = OP_READ_ID,                        .name = "READ_ID",              .command_1 = NAND_CMD_READ_ID,                            .command_2 = 0U,  .address_cycles = 1U, .data_input_cycles = 0U, .valid_while_selected_lun_busy = 0U, .valid_while_other_lun_busy = 0U, .has_data = 1U,                   .data_length = 5U,                           .uops = g_nand_read_id_uops,                  .timing = {NAND_TIME_TWB_US, 0U, NAND_TIME_TWH_US, NAND_TIME_TR_US}},
        [OP_READ_PARAMETER_PAGE]                    = {                   .operation = OP_READ_PARAMETER_PAGE,            .name = "READ_PARAMETER_PAGE",  .command_1 = NAND_CMD_READ_PARAMETER_PAGE,                            .command_2 = 0U,  .address_cycles = 1U, .data_input_cycles = 0U, .valid_while_selected_lun_busy = 0U, .valid_while_other_lun_busy = 0U, .has_data = 1U,                 .data_length = 256U,                      .uops = g_nand_read_parameter_page_uops,                  .timing = {NAND_TIME_TWB_US, 0U, NAND_TIME_TWH_US, NAND_TIME_TR_US}},
        [OP_READ_UNIQUE_ID]                         = {                        .operation = OP_READ_UNIQUE_ID,                 .name = "READ_UNIQUE_ID",       .command_1 = NAND_CMD_READ_UNIQUE_ID,                            .command_2 = 0U,  .address_cycles = 1U, .data_input_cycles = 0U, .valid_while_selected_lun_busy = 0U, .valid_while_other_lun_busy = 0U, .has_data = 1U,                 .data_length = 256U,                      .uops = g_nand_read_unique_id_uops,                  .timing = {NAND_TIME_TWB_US, 0U, NAND_TIME_TWH_US, NAND_TIME_TR_US}},
        [OP_GET_FEATURES]                           = {                          .operation = OP_GET_FEATURES,                   .name = "GET_FEATURES",         .command_1 = NAND_CMD_GET_FEATURES,                            .command_2 = 0U,  .address_cycles = 1U, .data_input_cycles = 0U, .valid_while_selected_lun_busy = 0U, .valid_while_other_lun_busy = 0U, .has_data = 1U,                   .data_length = 4U,                      .uops = g_nand_get_features_uops,                  .timing = {NAND_TIME_TWB_US, 0U, NAND_TIME_TWH_US, NAND_TIME_TR_US}},
        [OP_SET_FEATURES]                           = {                          .operation = OP_SET_FEATURES,                   .name = "SET_FEATURES",         .command_1 = NAND_CMD_SET_FEATURES,                            .command_2 = 0U,  .address_cycles = 1U, .data_input_cycles = 4U, .valid_while_selected_lun_busy = 0U, .valid_while_other_lun_busy = 0U, .has_data = 1U,                   .data_length = 4U,                      .uops = g_nand_set_features_uops,               .timing = {NAND_TIME_TWB_US, 0U, NAND_TIME_TWH_US, NAND_TIME_TPROG_US}},
        [OP_READ_STATUS]                            = {                           .operation = OP_READ_STATUS,                    .name = "READ_STATUS",          .command_1 = NAND_CMD_READ_STATUS,                            .command_2 = 0U,  .address_cycles = 0U, .data_input_cycles = 0U, .valid_while_selected_lun_busy = 0U, .valid_while_other_lun_busy = 1U, .has_data = 1U,                   .data_length = 1U,                       .uops = g_nand_read_status_uops,                  .timing = {NAND_TIME_TWB_US, 0U, NAND_TIME_TWH_US, NAND_TIME_TR_US}},
        [OP_READ_STATUS_ENHANCED]                   = {                  .operation = OP_READ_STATUS_ENHANCED,           .name = "READ_STATUS_ENHANCED", .command_1 = NAND_CMD_READ_STATUS_ENHANCED,                            .command_2 = 0U,  .address_cycles = 3U, .data_input_cycles = 0U, .valid_while_selected_lun_busy = 0U, .valid_while_other_lun_busy = 1U, .has_data = 1U,                   .data_length = 1U,              .uops = g_nand_read_status_enhanced_uops,                  .timing = {NAND_TIME_TWB_US, 0U, NAND_TIME_TWH_US, NAND_TIME_TR_US}},
        [OP_RANDOM_DATA_READ]                       = {                      .operation = OP_RANDOM_DATA_READ,               .name = "RANDOM_DATA_READ",     .command_1 = NAND_CMD_RANDOM_DATA_READ, .command_2 = NAND_CMD_RANDOM_DATA_READ_END,  .address_cycles = 2U, .data_input_cycles = 0U, .valid_while_selected_lun_busy = 0U, .valid_while_other_lun_busy = 1U, .has_data = 1U, .data_length = (PAGE_SIZE / CW_NUM),                       .uops = g_nand_random_read_uops,                  .timing = {NAND_TIME_TWB_US, 0U, NAND_TIME_TWH_US, NAND_TIME_TR_US}},
        [OP_PROGRAM_FOR_INTERNAL_DATA_MOVE]         = {        .operation = OP_PROGRAM_FOR_INTERNAL_DATA_MOVE, .name = "PROGRAM_FOR_INTERNAL_DATA_MOVE",    .command_1 = NAND_CMD_RANDOM_DATA_INPUT, .command_2 = NAND_CMD_PROGRAM_PAGE_CONFIRM,  .address_cycles = 5U, .data_input_cycles = 1U, .valid_while_selected_lun_busy = 0U, .valid_while_other_lun_busy = 1U, .has_data = 1U,                   .data_length = 1U,                      .uops = g_nand_program_move_uops,               .timing = {NAND_TIME_TWB_US, 0U, NAND_TIME_TWH_US, NAND_TIME_TPROG_US}},
        [OP_NORMAL_READ_PAGE_WITH_DATA_OUT]         = {        .operation = OP_NORMAL_READ_PAGE_WITH_DATA_OUT,           .name = "READ_PAGE_W_DATA_OUT",      .command_1 = NAND_CMD_READ_PAGE_SETUP,    .command_2 = NAND_CMD_READ_PAGE_CONFIRM,  .address_cycles = 5U, .data_input_cycles = 0U, .valid_while_selected_lun_busy = 0U, .valid_while_other_lun_busy = 1U, .has_data = 1U,            .data_length = PAGE_SIZE,                  .uops = g_nand_normal_read_page_uops,                  .timing = {NAND_TIME_TWB_US, 0U, NAND_TIME_TWH_US, NAND_TIME_TR_US}},
        [OP_NORMAL_READ_PAGE_WITHOUT_DATA_OUT]      = {     .operation = OP_NORMAL_READ_PAGE_WITHOUT_DATA_OUT,          .name = "READ_PAGE_WO_DATA_OUT",      .command_1 = NAND_CMD_READ_PAGE_SETUP,    .command_2 = NAND_CMD_READ_PAGE_CONFIRM,  .address_cycles = 5U, .data_input_cycles = 0U, .valid_while_selected_lun_busy = 0U, .valid_while_other_lun_busy = 1U, .has_data = 1U,            .data_length = PAGE_SIZE, .uops = g_nand_normal_read_page_without_data_out_uops,                  .timing = {NAND_TIME_TWB_US, 0U, NAND_TIME_TWH_US, NAND_TIME_TR_US}},
        [OP_READ_PAGE_CACHE_SEQ]                    = {                   .operation = OP_READ_PAGE_CACHE_SEQ,            .name = "READ_PAGE_CACHE_SEQ",  .command_1 = NAND_CMD_READ_PAGE_CACHE_SEQ,                            .command_2 = 0U,  .address_cycles = 0U, .data_input_cycles = 0U, .valid_while_selected_lun_busy = 0U, .valid_while_other_lun_busy = 0U, .has_data = 1U,            .data_length = PAGE_SIZE,                        .uops = g_nand_cache_read_seq_uops,                  .timing = {NAND_TIME_TWB_US, 0U, NAND_TIME_TWH_US, NAND_TIME_TR_US}},
        [OP_READ_PAGE_CACHE_RANDOM]                 = {                .operation = OP_READ_PAGE_CACHE_RANDOM,         .name = "READ_PAGE_CACHE_RANDOM",      .command_1 = NAND_CMD_READ_PAGE_SETUP,  .command_2 = NAND_CMD_READ_PAGE_CACHE_SEQ,  .address_cycles = 5U, .data_input_cycles = 0U, .valid_while_selected_lun_busy = 0U, .valid_while_other_lun_busy = 1U, .has_data = 1U,            .data_length = PAGE_SIZE,                 .uops = g_nand_cache_read_random_uops,                  .timing = {NAND_TIME_TWB_US, 0U, NAND_TIME_TWH_US, NAND_TIME_TR_US}},
        [OP_READ_PAGE_CACHE_LAST]                   = {                  .operation = OP_READ_PAGE_CACHE_LAST,           .name = "READ_PAGE_CACHE_LAST", .command_1 = NAND_CMD_READ_PAGE_CACHE_LAST,                            .command_2 = 0U,  .address_cycles = 0U, .data_input_cycles = 0U, .valid_while_selected_lun_busy = 0U, .valid_while_other_lun_busy = 0U, .has_data = 1U,            .data_length = PAGE_SIZE,                        .uops = g_nand_cache_read_last_uops,                  .timing = {NAND_TIME_TWB_US, 0U, NAND_TIME_TWH_US, NAND_TIME_TR_US}},
        [OP_READ_PAGE_CACHE_START]                  = {             .operation = OP_READ_PAGE_CACHE_START,           .name = "READ_PAGE_CACHE_START",      .command_1 = NAND_CMD_READ_PAGE_SETUP,    .command_2 = NAND_CMD_READ_PAGE_CONFIRM,  .address_cycles = 5U, .data_input_cycles = 0U, .valid_while_selected_lun_busy = 0U, .valid_while_other_lun_busy = 1U, .has_data = 1U,            .data_length = PAGE_SIZE, .uops = g_nand_cache_start_uops,                  .timing = {NAND_TIME_TWB_US, 0U, NAND_TIME_TWH_US, NAND_TIME_TR_US}},
        [OP_NORMAL_PROGRAM_PAGE]                    = {                   .operation = OP_NORMAL_PROGRAM_PAGE,                   .name = "PROGRAM_PAGE",   .command_1 = NAND_CMD_PROGRAM_PAGE_SETUP, .command_2 = NAND_CMD_PROGRAM_PAGE_CONFIRM,  .address_cycles = 5U, .data_input_cycles = 1U, .valid_while_selected_lun_busy = 0U, .valid_while_other_lun_busy = 1U, .has_data = 1U,            .data_length = PAGE_SIZE,               .uops = g_nand_normal_program_page_uops,               .timing = {NAND_TIME_TWB_US, 0U, NAND_TIME_TWH_US, NAND_TIME_TPROG_US}},
        [OP_PROGRAM_PAGE_CACHE]                     = {                    .operation = OP_PROGRAM_PAGE_CACHE,             .name = "PROGRAM_PAGE_CACHE",   .command_1 = NAND_CMD_PROGRAM_PAGE_SETUP,   .command_2 = NAND_CMD_PROGRAM_PAGE_CACHE,  .address_cycles = 5U, .data_input_cycles = 1U, .valid_while_selected_lun_busy = 0U, .valid_while_other_lun_busy = 1U, .has_data = 1U,            .data_length = PAGE_SIZE,                     .uops = g_nand_program_cache_uops,               .timing = {NAND_TIME_TWB_US, 0U, NAND_TIME_TWH_US, NAND_TIME_TPROG_US}},
        [OP_ERASE_BLOCK]                            = {                           .operation = OP_ERASE_BLOCK,                    .name = "ERASE_BLOCK",    .command_1 = NAND_CMD_BLOCK_ERASE_SETUP,  .command_2 = NAND_CMD_BLOCK_ERASE_CONFIRM,  .address_cycles = 3U, .data_input_cycles = 0U, .valid_while_selected_lun_busy = 0U, .valid_while_other_lun_busy = 1U, .has_data = 0U,                   .data_length = 0U,                       .uops = g_nand_erase_block_uops,               .timing = {NAND_TIME_TWB_US, 0U, NAND_TIME_TWH_US, NAND_TIME_TBERS_US}},
        [OP_READ_FOR_INTERNAL_DATA_MOVE]            = {           .operation = OP_READ_FOR_INTERNAL_DATA_MOVE,    .name = "READ_FOR_INTERNAL_DATA_MOVE",      .command_1 = NAND_CMD_READ_PAGE_SETUP,   .command_2 = NAND_CMD_INTERNAL_DATA_MOVE,  .address_cycles = 5U, .data_input_cycles = 0U, .valid_while_selected_lun_busy = 0U, .valid_while_other_lun_busy = 1U, .has_data = 1U,                   .data_length = 0U,                     .uops = g_nand_internal_read_uops,                  .timing = {NAND_TIME_TWB_US, 0U, NAND_TIME_TWH_US, NAND_TIME_TR_US}},
        [OP_PROGRAM_FOR_INTERNAL_MOVE]              = {             .operation = OP_PROGRAM_FOR_INTERNAL_MOVE,      .name = "PROGRAM_FOR_INTERNAL_MOVE",    .command_1 = NAND_CMD_RANDOM_DATA_INPUT, .command_2 = NAND_CMD_PROGRAM_PAGE_CONFIRM,  .address_cycles = 5U, .data_input_cycles = 0U, .valid_while_selected_lun_busy = 0U, .valid_while_other_lun_busy = 1U, .has_data = 1U,                   .data_length = 0U,                  .uops = g_nand_internal_program_uops,               .timing = {NAND_TIME_TWB_US, 0U, NAND_TIME_TWH_US, NAND_TIME_TPROG_US}},
        [OP_BLOCK_UNLOCK_LOW]                       = {                      .operation = OP_BLOCK_UNLOCK_LOW,               .name = "BLOCK_UNLOCK_LOW",     .command_1 = NAND_CMD_BLOCK_UNLOCK_LOW,                            .command_2 = 0U,  .address_cycles = 3U, .data_input_cycles = 0U, .valid_while_selected_lun_busy = 0U, .valid_while_other_lun_busy = 0U, .has_data = 0U,                   .data_length = 0U,                     .uops = g_nand_block_unlock_low_uops,                  .timing = {NAND_TIME_TWB_US, 0U, NAND_TIME_TWH_US, NAND_TIME_TR_US}},
        [OP_BLOCK_UNLOCK_HIGH]                      = {                     .operation = OP_BLOCK_UNLOCK_HIGH,              .name = "BLOCK_UNLOCK_HIGH",    .command_1 = NAND_CMD_BLOCK_UNLOCK_HIGH,                            .command_2 = 0U,  .address_cycles = 3U, .data_input_cycles = 0U, .valid_while_selected_lun_busy = 0U, .valid_while_other_lun_busy = 0U, .has_data = 0U,                   .data_length = 0U,                     .uops = g_nand_block_unlock_high_uops,                  .timing = {NAND_TIME_TWB_US, 0U, NAND_TIME_TWH_US, NAND_TIME_TR_US}},
        [OP_BLOCK_LOCK]                             = {                            .operation = OP_BLOCK_LOCK,                     .name = "BLOCK_LOCK",           .command_1 = NAND_CMD_BLOCK_LOCK,                            .command_2 = 0U,  .address_cycles = 0U, .data_input_cycles = 0U, .valid_while_selected_lun_busy = 0U, .valid_while_other_lun_busy = 0U, .has_data = 0U,                   .data_length = 0U,                     .uops = g_nand_block_lock_uops,                  .timing = {NAND_TIME_TWB_US, 0U, NAND_TIME_TWH_US, NAND_TIME_TR_US}},
        [OP_BLOCK_LOCK_TIGHT]                       = {                      .operation = OP_BLOCK_LOCK_TIGHT,               .name = "BLOCK_LOCK_TIGHT",     .command_1 = NAND_CMD_BLOCK_LOCK_TIGHT,                            .command_2 = 0U,  .address_cycles = 0U, .data_input_cycles = 0U, .valid_while_selected_lun_busy = 0U, .valid_while_other_lun_busy = 0U, .has_data = 0U,                   .data_length = 0U,                     .uops = g_nand_block_lock_tight_uops,                  .timing = {NAND_TIME_TWB_US, 0U, NAND_TIME_TWH_US, NAND_TIME_TR_US}},
        [OP_BLOCK_LOCK_STATUS]                      = {                     .operation = OP_BLOCK_LOCK_STATUS,              .name = "BLOCK_LOCK_STATUS",    .command_1 = NAND_CMD_BLOCK_LOCK_STATUS,                            .command_2 = 0U,  .address_cycles = 3U, .data_input_cycles = 0U, .valid_while_selected_lun_busy = 0U, .valid_while_other_lun_busy = 0U, .has_data = 1U,                   .data_length = 1U,                 .uops = g_nand_block_lock_status_uops,                  .timing = {NAND_TIME_TWB_US, 0U, NAND_TIME_TWH_US, NAND_TIME_TR_US}},
        [OP_OTP_DATA_LOCK]                          = {                         .operation = OP_OTP_DATA_LOCK,                  .name = "OTP_DATA_LOCK",   .command_1 = NAND_CMD_PROGRAM_PAGE_SETUP, .command_2 = NAND_CMD_PROGRAM_PAGE_CONFIRM,  .address_cycles = 5U, .data_input_cycles = 0U, .valid_while_selected_lun_busy = 0U, .valid_while_other_lun_busy = 0U, .has_data = 0U,                   .data_length = 0U,                          .uops = g_nand_otp_lock_uops,               .timing = {NAND_TIME_TWB_US, 0U, NAND_TIME_TWH_US, NAND_TIME_TPROG_US}},
        [OP_OTP_DATA_PROGRAM]                       = {                      .operation = OP_OTP_DATA_PROGRAM,               .name = "OTP_DATA_PROGRAM",   .command_1 = NAND_CMD_PROGRAM_PAGE_SETUP, .command_2 = NAND_CMD_PROGRAM_PAGE_CONFIRM,  .address_cycles = 5U, .data_input_cycles = 1U, .valid_while_selected_lun_busy = 0U, .valid_while_other_lun_busy = 0U, .has_data = 1U,            .data_length = PAGE_SIZE,               .uops = g_nand_normal_program_page_uops,               .timing = {NAND_TIME_TWB_US, 0U, NAND_TIME_TWH_US, NAND_TIME_TPROG_US}},
        [OP_OTP_DATA_READ]                          = {                         .operation = OP_OTP_DATA_READ,                  .name = "OTP_DATA_READ",      .command_1 = NAND_CMD_READ_PAGE_SETUP,    .command_2 = NAND_CMD_READ_PAGE_CONFIRM,  .address_cycles = 5U, .data_input_cycles = 0U, .valid_while_selected_lun_busy = 0U, .valid_while_other_lun_busy = 0U, .has_data = 1U,            .data_length = PAGE_SIZE,                  .uops = g_nand_normal_read_page_uops,                  .timing = {NAND_TIME_TWB_US, 0U, NAND_TIME_TWH_US, NAND_TIME_TR_US}},
        [OP_READ_PAGE_MULTI_PLANE_WITHOUT_DATA_OUT] = {.operation = OP_READ_PAGE_MULTI_PLANE_WITHOUT_DATA_OUT,            .name = "MP_READ_WO_DATA_OUT",      .command_1 = NAND_CMD_READ_PAGE_SETUP,    .command_2 = NAND_CMD_READ_PAGE_CONFIRM, .address_cycles = 10U, .data_input_cycles = 0U, .valid_while_selected_lun_busy = 0U, .valid_while_other_lun_busy = 1U, .has_data = 1U,     .data_length = PAGE_BUFFER_SIZE, .uops = g_nand_multi_plane_read_without_data_out_uops,                  .timing = {NAND_TIME_TWB_US, 0U, NAND_TIME_TWH_US, NAND_TIME_TR_US}},
        [OP_READ_PAGE_MULTI_PLANE_WITH_DATA_OUT]    = {   .operation = OP_READ_PAGE_MULTI_PLANE_WITH_DATA_OUT,             .name = "MP_READ_W_DATA_OUT",      .command_1 = NAND_CMD_READ_PAGE_SETUP,    .command_2 = NAND_CMD_READ_PAGE_CONFIRM, .address_cycles = 10U, .data_input_cycles = 0U, .valid_while_selected_lun_busy = 0U, .valid_while_other_lun_busy = 1U, .has_data = 1U,     .data_length = PAGE_BUFFER_SIZE,    .uops = g_nand_multi_plane_read_with_data_out_uops,                  .timing = {NAND_TIME_TWB_US, 0U, NAND_TIME_TWH_US, NAND_TIME_TR_US}},
        [OP_PROGRAM_PAGE_MULTI_PLANE]               = {              .operation = OP_PROGRAM_PAGE_MULTI_PLANE,       .name = "PROGRAM_PAGE_MULTI_PLANE",   .command_1 = NAND_CMD_PROGRAM_PAGE_SETUP, .command_2 = NAND_CMD_PROGRAM_PAGE_CONFIRM, .address_cycles = 10U, .data_input_cycles = 2U, .valid_while_selected_lun_busy = 0U, .valid_while_other_lun_busy = 1U, .has_data = 1U,     .data_length = PAGE_BUFFER_SIZE,               .uops = g_nand_multi_plane_program_uops,               .timing = {NAND_TIME_TWB_US, 0U, NAND_TIME_TWH_US, NAND_TIME_TPROG_US}},
        [OP_ERASE_BLOCK_MULTI_PLANE]                = {               .operation = OP_ERASE_BLOCK_MULTI_PLANE,        .name = "ERASE_BLOCK_MULTI_PLANE",    .command_1 = NAND_CMD_BLOCK_ERASE_SETUP,  .command_2 = NAND_CMD_BLOCK_ERASE_CONFIRM,  .address_cycles = 6U, .data_input_cycles = 0U, .valid_while_selected_lun_busy = 0U, .valid_while_other_lun_busy = 1U, .has_data = 0U,                   .data_length = 0U,                 .uops = g_nand_multi_plane_erase_uops,               .timing = {NAND_TIME_TWB_US, 0U, NAND_TIME_TWH_US, NAND_TIME_TBERS_US}},
};

/* nand_operation_has_data / nand_operation_data_length 已由 onfi_1_0.h 中的宏替代，
 * 直接读 g_nand_operation_map[op].has_data / .data_length。
 * 删除前请确认所有外部引用已清理。 */