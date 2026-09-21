#include "onfi_1_0.h"

#include "nand_config.h"

#include <stddef.h>

#define NAND_TIME_TWB_US   (1U)
#define NAND_TIME_TRST_US  (500U)
#define NAND_TIME_TPROG_US (300U)
#define NAND_TIME_TBERS_US (1050U)
#define NAND_TIME_TR_US    (38U)
#define NAND_TIME_TWH_US   (1U)

static const nand_uop_t g_nand_normal_read_page_uops[] =
    {
        { NAND_UOP_WAIT_READY,                         0U},
        {NAND_UOP_READ_STATUS,                         0U},
        {NAND_UOP_CHECK_READY,                         0U},
        {    NAND_UOP_COMMAND,   NAND_CMD_READ_PAGE_SETUP},
        {   NAND_UOP_COLUMN_0,                         0U},
        {   NAND_UOP_COLUMN_1,                         0U},
        {      NAND_UOP_ROW_0,                         0U},
        {      NAND_UOP_ROW_1,                         0U},
        {      NAND_UOP_ROW_2,                         0U},
        {    NAND_UOP_COMMAND, NAND_CMD_READ_PAGE_CONFIRM},
        { NAND_UOP_WAIT_READY,                         0U},
        {   NAND_UOP_DATA_OUT,                         0U},
        {NAND_UOP_READ_STATUS,                         0U},
        {NAND_UOP_CHECK_READY,                         0U},
        { NAND_UOP_CHECK_FAIL,                         0U},
        {        NAND_UOP_END,                         0U},
};

static const nand_uop_t g_nand_normal_program_page_uops[] =
    {
        { NAND_UOP_WAIT_READY,                            0U},
        {NAND_UOP_READ_STATUS,                            0U},
        {NAND_UOP_CHECK_READY,                            0U},
        {    NAND_UOP_COMMAND,   NAND_CMD_PROGRAM_PAGE_SETUP},
        {   NAND_UOP_COLUMN_0,                            0U},
        {   NAND_UOP_COLUMN_1,                            0U},
        {      NAND_UOP_ROW_0,                            0U},
        {      NAND_UOP_ROW_1,                            0U},
        {      NAND_UOP_ROW_2,                            0U},
        {    NAND_UOP_DATA_IN,                            0U},
        {    NAND_UOP_COMMAND, NAND_CMD_PROGRAM_PAGE_CONFIRM},
        { NAND_UOP_WAIT_READY,                            0U},
        {NAND_UOP_READ_STATUS,                            0U},
        {NAND_UOP_CHECK_READY,                            0U},
        { NAND_UOP_CHECK_FAIL,                            0U},
        {        NAND_UOP_END,                            0U},
};

static const nand_uop_t g_nand_erase_block_uops[] =
    {
        { NAND_UOP_WAIT_READY,                           0U},
        {NAND_UOP_READ_STATUS,                           0U},
        {NAND_UOP_CHECK_READY,                           0U},
        {    NAND_UOP_COMMAND,   NAND_CMD_BLOCK_ERASE_SETUP},
        {      NAND_UOP_ROW_0,                           0U},
        {      NAND_UOP_ROW_1,                           0U},
        {      NAND_UOP_ROW_2,                           0U},
        {    NAND_UOP_COMMAND, NAND_CMD_BLOCK_ERASE_CONFIRM},
        { NAND_UOP_WAIT_READY,                           0U},
        {NAND_UOP_READ_STATUS,                           0U},
        {NAND_UOP_CHECK_READY,                           0U},
        { NAND_UOP_CHECK_FAIL,                           0U},
        {        NAND_UOP_END,                           0U},
};

static const nand_uop_t g_nand_reset_uops[] = {
    {     NAND_UOP_WAIT_READY, 0U},
    {NAND_UOP_COMMAND_PRIMARY, 0U},
    {     NAND_UOP_WAIT_READY, 0U},
    {    NAND_UOP_READ_STATUS, 0U},
    {    NAND_UOP_CHECK_READY, 0U},
    {     NAND_UOP_CHECK_FAIL, 0U},
    {            NAND_UOP_END, 0U}
};
static const nand_uop_t g_nand_read_address_uops[] = {
    {     NAND_UOP_WAIT_READY, 0U},
    {    NAND_UOP_READ_STATUS, 0U},
    {    NAND_UOP_CHECK_READY, 0U},
    {NAND_UOP_COMMAND_PRIMARY, 0U},
    {       NAND_UOP_COLUMN_0, 0U},
    {       NAND_UOP_DATA_OUT, 0U},
    {    NAND_UOP_READ_STATUS, 0U},
    {    NAND_UOP_CHECK_READY, 0U},
    {            NAND_UOP_END, 0U}
};
/*
 * Read ID is the board bring-up command. Keep it identical to the vendor reference:
 * wait for R/B#, then 90h -> 00h -> data. Do not prepend 70h status polling here,
 * because an all-identical result is otherwise indistinguishable from stale status data.
 */
static const nand_uop_t g_nand_read_id_uops[] = {
    {     NAND_UOP_WAIT_READY, 0U},
    {NAND_UOP_COMMAND_PRIMARY, 0U},
    {       NAND_UOP_COLUMN_0, 0U},
    {        NAND_UOP_DATA_OUT, 0U},
    {            NAND_UOP_END, 0U}
};
static const nand_uop_t g_nand_set_features_uops[] = {
    {     NAND_UOP_WAIT_READY, 0U},
    {    NAND_UOP_READ_STATUS, 0U},
    {    NAND_UOP_CHECK_READY, 0U},
    {NAND_UOP_COMMAND_PRIMARY, 0U},
    {       NAND_UOP_COLUMN_0, 0U},
    {        NAND_UOP_DATA_IN, 0U},
    {     NAND_UOP_WAIT_READY, 0U},
    {    NAND_UOP_READ_STATUS, 0U},
    {    NAND_UOP_CHECK_READY, 0U},
    {     NAND_UOP_CHECK_FAIL, 0U},
    {            NAND_UOP_END, 0U}
};
static const nand_uop_t g_nand_read_status_uops[] = {
    {NAND_UOP_COMMAND_PRIMARY, 0U},
    {NAND_UOP_DATA_OUT_STATUS, 0U},
    {            NAND_UOP_END, 0U}
};
static const nand_uop_t g_nand_read_status_enhanced_uops[] = {
    {     NAND_UOP_WAIT_READY, 0U},
    {    NAND_UOP_READ_STATUS, 0U},
    {    NAND_UOP_CHECK_READY, 0U},
    {NAND_UOP_COMMAND_PRIMARY, 0U},
    {          NAND_UOP_ROW_0, 0U},
    {          NAND_UOP_ROW_1, 0U},
    {          NAND_UOP_ROW_2, 0U},
    {NAND_UOP_DATA_OUT_STATUS, 0U},
    {            NAND_UOP_END, 0U}
};
static const nand_uop_t g_nand_random_read_uops[] = {
    {       NAND_UOP_WAIT_READY, 0U},
    {      NAND_UOP_READ_STATUS, 0U},
    {      NAND_UOP_CHECK_READY, 0U},
    {  NAND_UOP_COMMAND_PRIMARY, 0U},
    {         NAND_UOP_COLUMN_0, 0U},
    {         NAND_UOP_COLUMN_1, 0U},
    {NAND_UOP_COMMAND_SECONDARY, 0U},
    {       NAND_UOP_WAIT_READY, 0U},
    {         NAND_UOP_DATA_OUT, 0U},
    {      NAND_UOP_READ_STATUS, 0U},
    {      NAND_UOP_CHECK_READY, 0U},
    {              NAND_UOP_END, 0U}
};
static const nand_uop_t g_nand_random_input_uops[] = {
    {     NAND_UOP_WAIT_READY, 0U},
    {    NAND_UOP_READ_STATUS, 0U},
    {    NAND_UOP_CHECK_READY, 0U},
    {NAND_UOP_COMMAND_PRIMARY, 0U},
    {       NAND_UOP_COLUMN_0, 0U},
    {       NAND_UOP_COLUMN_1, 0U},
    {        NAND_UOP_DATA_IN, 0U},
    {    NAND_UOP_READ_STATUS, 0U},
    {    NAND_UOP_CHECK_READY, 0U},
    {            NAND_UOP_END, 0U}
};
static const nand_uop_t g_nand_program_move_uops[] = {
    {       NAND_UOP_WAIT_READY, 0U},
    {      NAND_UOP_READ_STATUS, 0U},
    {      NAND_UOP_CHECK_READY, 0U},
    {  NAND_UOP_COMMAND_PRIMARY, 0U},
    {         NAND_UOP_COLUMN_0, 0U},
    {         NAND_UOP_COLUMN_1, 0U},
    {            NAND_UOP_ROW_0, 0U},
    {            NAND_UOP_ROW_1, 0U},
    {            NAND_UOP_ROW_2, 0U},
    {          NAND_UOP_DATA_IN, 0U},
    {NAND_UOP_COMMAND_SECONDARY, 0U},
    {       NAND_UOP_WAIT_READY, 0U},
    {      NAND_UOP_READ_STATUS, 0U},
    {      NAND_UOP_CHECK_READY, 0U},
    {       NAND_UOP_CHECK_FAIL, 0U},
    {              NAND_UOP_END, 0U}
};
static const nand_uop_t g_nand_read_mode_uops[] = {
    {     NAND_UOP_WAIT_READY, 0U},
    {    NAND_UOP_READ_STATUS, 0U},
    {    NAND_UOP_CHECK_READY, 0U},
    {NAND_UOP_COMMAND_PRIMARY, 0U},
    {    NAND_UOP_READ_STATUS, 0U},
    {    NAND_UOP_CHECK_READY, 0U},
    {            NAND_UOP_END, 0U}
};
static const nand_uop_t g_nand_cache_read_uops[] = {
    {     NAND_UOP_WAIT_READY, 0U},
    {    NAND_UOP_READ_STATUS, 0U},
    {    NAND_UOP_CHECK_READY, 0U},
    {NAND_UOP_COMMAND_PRIMARY, 0U},
    {     NAND_UOP_WAIT_READY, 0U},
    {       NAND_UOP_DATA_OUT, 0U},
    {    NAND_UOP_READ_STATUS, 0U},
    {    NAND_UOP_CHECK_READY, 0U},
    {            NAND_UOP_END, 0U}
};
static const nand_uop_t g_nand_cache_read_random_uops[] = {
    {       NAND_UOP_WAIT_READY, 0U},
    {      NAND_UOP_READ_STATUS, 0U},
    {      NAND_UOP_CHECK_READY, 0U},
    {  NAND_UOP_COMMAND_PRIMARY, 0U},
    {         NAND_UOP_COLUMN_0, 0U},
    {         NAND_UOP_COLUMN_1, 0U},
    {            NAND_UOP_ROW_0, 0U},
    {            NAND_UOP_ROW_1, 0U},
    {            NAND_UOP_ROW_2, 0U},
    {NAND_UOP_COMMAND_SECONDARY, 0U},
    {       NAND_UOP_WAIT_READY, 0U},
    {         NAND_UOP_DATA_OUT, 0U},
    {      NAND_UOP_READ_STATUS, 0U},
    {      NAND_UOP_CHECK_READY, 0U},
    {              NAND_UOP_END, 0U}
};
static const nand_uop_t g_nand_program_cache_uops[] = {
    {       NAND_UOP_WAIT_READY, 0U},
    {      NAND_UOP_READ_STATUS, 0U},
    {      NAND_UOP_CHECK_READY, 0U},
    {  NAND_UOP_COMMAND_PRIMARY, 0U},
    {         NAND_UOP_COLUMN_0, 0U},
    {         NAND_UOP_COLUMN_1, 0U},
    {            NAND_UOP_ROW_0, 0U},
    {            NAND_UOP_ROW_1, 0U},
    {            NAND_UOP_ROW_2, 0U},
    {          NAND_UOP_DATA_IN, 0U},
    {NAND_UOP_COMMAND_SECONDARY, 0U},
    {       NAND_UOP_WAIT_READY, 0U},
    {      NAND_UOP_READ_STATUS, 0U},
    {      NAND_UOP_CHECK_READY, 0U},
    {       NAND_UOP_CHECK_FAIL, 0U},
    {              NAND_UOP_END, 0U}
};
static const nand_uop_t g_nand_internal_read_uops[] = {
    {       NAND_UOP_WAIT_READY, 0U},
    {      NAND_UOP_READ_STATUS, 0U},
    {      NAND_UOP_CHECK_READY, 0U},
    {  NAND_UOP_COMMAND_PRIMARY, 0U},
    {         NAND_UOP_COLUMN_0, 0U},
    {         NAND_UOP_COLUMN_1, 0U},
    {            NAND_UOP_ROW_0, 0U},
    {            NAND_UOP_ROW_1, 0U},
    {            NAND_UOP_ROW_2, 0U},
    {NAND_UOP_COMMAND_SECONDARY, 0U},
    {       NAND_UOP_WAIT_READY, 0U},
    {      NAND_UOP_READ_STATUS, 0U},
    {      NAND_UOP_CHECK_READY, 0U},
    {       NAND_UOP_CHECK_FAIL, 0U},
    {              NAND_UOP_END, 0U}
};
static const nand_uop_t g_nand_internal_program_uops[] = {
    {       NAND_UOP_WAIT_READY, 0U},
    {      NAND_UOP_READ_STATUS, 0U},
    {      NAND_UOP_CHECK_READY, 0U},
    {  NAND_UOP_COMMAND_PRIMARY, 0U},
    {         NAND_UOP_COLUMN_0, 0U},
    {         NAND_UOP_COLUMN_1, 0U},
    {            NAND_UOP_ROW_0, 0U},
    {            NAND_UOP_ROW_1, 0U},
    {            NAND_UOP_ROW_2, 0U},
    {NAND_UOP_COMMAND_SECONDARY, 0U},
    {       NAND_UOP_WAIT_READY, 0U},
    {      NAND_UOP_READ_STATUS, 0U},
    {      NAND_UOP_CHECK_READY, 0U},
    {       NAND_UOP_CHECK_FAIL, 0U},
    {              NAND_UOP_END, 0U}
};
static const nand_uop_t g_nand_block_address_uops[] = {
    {     NAND_UOP_WAIT_READY, 0U},
    {    NAND_UOP_READ_STATUS, 0U},
    {    NAND_UOP_CHECK_READY, 0U},
    {NAND_UOP_COMMAND_PRIMARY, 0U},
    {          NAND_UOP_ROW_0, 0U},
    {          NAND_UOP_ROW_1, 0U},
    {          NAND_UOP_ROW_2, 0U},
    {    NAND_UOP_READ_STATUS, 0U},
    {    NAND_UOP_CHECK_READY, 0U},
    {            NAND_UOP_END, 0U}
};
static const nand_uop_t g_nand_block_command_uops[] = {
    {     NAND_UOP_WAIT_READY, 0U},
    {    NAND_UOP_READ_STATUS, 0U},
    {    NAND_UOP_CHECK_READY, 0U},
    {NAND_UOP_COMMAND_PRIMARY, 0U},
    {    NAND_UOP_READ_STATUS, 0U},
    {    NAND_UOP_CHECK_READY, 0U},
    {            NAND_UOP_END, 0U}
};
static const nand_uop_t g_nand_block_lock_status_uops[] = {
    {     NAND_UOP_WAIT_READY, 0U},
    {    NAND_UOP_READ_STATUS, 0U},
    {    NAND_UOP_CHECK_READY, 0U},
    {NAND_UOP_COMMAND_PRIMARY, 0U},
    {          NAND_UOP_ROW_0, 0U},
    {          NAND_UOP_ROW_1, 0U},
    {          NAND_UOP_ROW_2, 0U},
    {NAND_UOP_DATA_OUT_STATUS, 0U},
    {            NAND_UOP_END, 0U}
};
static const nand_uop_t g_nand_otp_lock_uops[] = {
    {       NAND_UOP_WAIT_READY, 0U},
    {      NAND_UOP_READ_STATUS, 0U},
    {      NAND_UOP_CHECK_READY, 0U},
    {  NAND_UOP_COMMAND_PRIMARY, 0U},
    {         NAND_UOP_COLUMN_0, 0U},
    {         NAND_UOP_COLUMN_1, 0U},
    {            NAND_UOP_ROW_0, 0U},
    {            NAND_UOP_ROW_1, 0U},
    {            NAND_UOP_ROW_2, 0U},
    {NAND_UOP_COMMAND_SECONDARY, 0U},
    {       NAND_UOP_WAIT_READY, 0U},
    {      NAND_UOP_READ_STATUS, 0U},
    {      NAND_UOP_CHECK_READY, 0U},
    {       NAND_UOP_CHECK_FAIL, 0U},
    {              NAND_UOP_END, 0U}
};

const nand_operation_map_t g_nand_operation_map[OP_COUNT] =
    {
        [OP_RESET]                          = {                         OP_RESET,                          "RESET",           NAND_ONFI_CMD_RESET,                            0U, 0U, 0U, 1U, 1U,                g_nand_reset_uops, {NAND_TIME_TWB_US, NAND_TIME_TRST_US, NAND_TIME_TWH_US, NAND_TIME_TRST_US}},
        [OP_READ_ID]                        = {                       OP_READ_ID,                        "READ_ID",              NAND_CMD_READ_ID,                            0U, 1U, 0U, 0U, 0U,              g_nand_read_id_uops,                  {NAND_TIME_TWB_US, 0U, NAND_TIME_TWH_US, NAND_TIME_TR_US}},
        [OP_READ_PARAMETER_PAGE]            = {           OP_READ_PARAMETER_PAGE,            "READ_PARAMETER_PAGE",  NAND_CMD_READ_PARAMETER_PAGE,                            0U, 1U, 0U, 0U, 0U,         g_nand_read_address_uops,                  {NAND_TIME_TWB_US, 0U, NAND_TIME_TWH_US, NAND_TIME_TR_US}},
        [OP_READ_UNIQUE_ID]                 = {                OP_READ_UNIQUE_ID,                 "READ_UNIQUE_ID",       NAND_CMD_READ_UNIQUE_ID,                            0U, 1U, 0U, 0U, 0U,         g_nand_read_address_uops,                  {NAND_TIME_TWB_US, 0U, NAND_TIME_TWH_US, NAND_TIME_TR_US}},
        [OP_GET_FEATURES]                   = {                  OP_GET_FEATURES,                   "GET_FEATURES",         NAND_CMD_GET_FEATURES,                            0U, 1U, 0U, 0U, 0U,         g_nand_read_address_uops,                  {NAND_TIME_TWB_US, 0U, NAND_TIME_TWH_US, NAND_TIME_TR_US}},
        [OP_SET_FEATURES]                   = {                  OP_SET_FEATURES,                   "SET_FEATURES",         NAND_CMD_SET_FEATURES,                            0U, 1U, 4U, 0U, 0U,         g_nand_set_features_uops,               {NAND_TIME_TWB_US, 0U, NAND_TIME_TWH_US, NAND_TIME_TPROG_US}},
        [OP_READ_STATUS]                    = {                   OP_READ_STATUS,                    "READ_STATUS",          NAND_CMD_READ_STATUS,                            0U, 0U, 0U, 1U, 0U,          g_nand_read_status_uops,                  {NAND_TIME_TWB_US, 0U, NAND_TIME_TWH_US, NAND_TIME_TR_US}},
        [OP_READ_STATUS_ENHANCED]           = {          OP_READ_STATUS_ENHANCED,           "READ_STATUS_ENHANCED", NAND_CMD_READ_STATUS_ENHANCED,                            0U, 3U, 0U, 1U, 1U, g_nand_read_status_enhanced_uops,                  {NAND_TIME_TWB_US, 0U, NAND_TIME_TWH_US, NAND_TIME_TR_US}},
        [OP_RANDOM_DATA_READ]               = {              OP_RANDOM_DATA_READ,               "RANDOM_DATA_READ",     NAND_CMD_RANDOM_DATA_READ, NAND_CMD_RANDOM_DATA_READ_END, 2U, 0U, 0U, 1U,          g_nand_random_read_uops,                  {NAND_TIME_TWB_US, 0U, NAND_TIME_TWH_US, NAND_TIME_TR_US}},
        [OP_RANDOM_DATA_INPUT]              = {             OP_RANDOM_DATA_INPUT,              "RANDOM_DATA_INPUT",    NAND_CMD_RANDOM_DATA_INPUT,                            0U, 2U, 1U, 0U, 1U,         g_nand_random_input_uops,               {NAND_TIME_TWB_US, 0U, NAND_TIME_TWH_US, NAND_TIME_TPROG_US}},
        [OP_PROGRAM_FOR_INTERNAL_DATA_MOVE] = {OP_PROGRAM_FOR_INTERNAL_DATA_MOVE, "PROGRAM_FOR_INTERNAL_DATA_MOVE",    NAND_CMD_RANDOM_DATA_INPUT, NAND_CMD_PROGRAM_PAGE_CONFIRM, 5U, 1U, 0U, 1U,         g_nand_program_move_uops,               {NAND_TIME_TWB_US, 0U, NAND_TIME_TWH_US, NAND_TIME_TPROG_US}},
        [OP_READ_MODE]                      = {                     OP_READ_MODE,                      "READ_MODE",            NAND_CMD_READ_MODE,                            0U, 0U, 0U, 0U, 1U,            g_nand_read_mode_uops,                  {NAND_TIME_TWB_US, 0U, NAND_TIME_TWH_US, NAND_TIME_TR_US}},
        [OP_NORMAL_READ_PAGE]               = {              OP_NORMAL_READ_PAGE,                      "READ_PAGE",      NAND_CMD_READ_PAGE_SETUP,    NAND_CMD_READ_PAGE_CONFIRM, 5U, 0U, 0U, 1U,     g_nand_normal_read_page_uops,                  {NAND_TIME_TWB_US, 0U, NAND_TIME_TWH_US, NAND_TIME_TR_US}},
        [OP_READ_PAGE_CACHE_SEQ]            = {           OP_READ_PAGE_CACHE_SEQ,            "READ_PAGE_CACHE_SEQ",  NAND_CMD_READ_PAGE_CACHE_SEQ,                            0U, 0U, 0U, 0U, 1U,           g_nand_cache_read_uops,                  {NAND_TIME_TWB_US, 0U, NAND_TIME_TWH_US, NAND_TIME_TR_US}},
        [OP_READ_PAGE_CACHE_RANDOM]         = {        OP_READ_PAGE_CACHE_RANDOM,         "READ_PAGE_CACHE_RANDOM",      NAND_CMD_READ_PAGE_SETUP,  NAND_CMD_READ_PAGE_CACHE_SEQ, 5U, 0U, 0U, 1U,    g_nand_cache_read_random_uops,                  {NAND_TIME_TWB_US, 0U, NAND_TIME_TWH_US, NAND_TIME_TR_US}},
        [OP_READ_PAGE_CACHE_LAST]           = {          OP_READ_PAGE_CACHE_LAST,           "READ_PAGE_CACHE_LAST", NAND_CMD_READ_PAGE_CACHE_LAST,                            0U, 0U, 0U, 0U, 1U,           g_nand_cache_read_uops,                  {NAND_TIME_TWB_US, 0U, NAND_TIME_TWH_US, NAND_TIME_TR_US}},
        [OP_NORMAL_PROGRAM_PAGE]            = {           OP_NORMAL_PROGRAM_PAGE,                   "PROGRAM_PAGE",   NAND_CMD_PROGRAM_PAGE_SETUP, NAND_CMD_PROGRAM_PAGE_CONFIRM, 5U, 1U, 0U, 1U,  g_nand_normal_program_page_uops,               {NAND_TIME_TWB_US, 0U, NAND_TIME_TWH_US, NAND_TIME_TPROG_US}},
        [OP_PROGRAM_PAGE_CACHE]             = {            OP_PROGRAM_PAGE_CACHE,             "PROGRAM_PAGE_CACHE",   NAND_CMD_PROGRAM_PAGE_SETUP,   NAND_CMD_PROGRAM_PAGE_CACHE, 5U, 1U, 0U, 1U,        g_nand_program_cache_uops,               {NAND_TIME_TWB_US, 0U, NAND_TIME_TWH_US, NAND_TIME_TPROG_US}},
        [OP_ERASE_BLOCK]                    = {                   OP_ERASE_BLOCK,                    "ERASE_BLOCK",    NAND_CMD_BLOCK_ERASE_SETUP,  NAND_CMD_BLOCK_ERASE_CONFIRM, 3U, 0U, 0U, 1U,          g_nand_erase_block_uops,               {NAND_TIME_TWB_US, 0U, NAND_TIME_TWH_US, NAND_TIME_TBERS_US}},
        [OP_READ_FOR_INTERNAL_DATA_MOVE]    = {   OP_READ_FOR_INTERNAL_DATA_MOVE,    "READ_FOR_INTERNAL_DATA_MOVE",      NAND_CMD_READ_PAGE_SETUP,   NAND_CMD_INTERNAL_DATA_MOVE, 5U, 0U, 0U, 1U,        g_nand_internal_read_uops,                  {NAND_TIME_TWB_US, 0U, NAND_TIME_TWH_US, NAND_TIME_TR_US}},
        [OP_PROGRAM_FOR_INTERNAL_MOVE]      = {     OP_PROGRAM_FOR_INTERNAL_MOVE,      "PROGRAM_FOR_INTERNAL_MOVE",    NAND_CMD_RANDOM_DATA_INPUT, NAND_CMD_PROGRAM_PAGE_CONFIRM, 5U, 1U, 0U, 1U,     g_nand_internal_program_uops,               {NAND_TIME_TWB_US, 0U, NAND_TIME_TWH_US, NAND_TIME_TPROG_US}},
        [OP_BLOCK_UNLOCK_LOW]               = {              OP_BLOCK_UNLOCK_LOW,               "BLOCK_UNLOCK_LOW",     NAND_CMD_BLOCK_UNLOCK_LOW,                            0U, 3U, 0U, 0U, 1U,        g_nand_block_address_uops,                  {NAND_TIME_TWB_US, 0U, NAND_TIME_TWH_US, NAND_TIME_TR_US}},
        [OP_BLOCK_UNLOCK_HIGH]              = {             OP_BLOCK_UNLOCK_HIGH,              "BLOCK_UNLOCK_HIGH",    NAND_CMD_BLOCK_UNLOCK_HIGH,                            0U, 3U, 0U, 0U, 1U,        g_nand_block_address_uops,                  {NAND_TIME_TWB_US, 0U, NAND_TIME_TWH_US, NAND_TIME_TR_US}},
        [OP_BLOCK_LOCK]                     = {                    OP_BLOCK_LOCK,                     "BLOCK_LOCK",           NAND_CMD_BLOCK_LOCK,                            0U, 0U, 0U, 0U, 1U,        g_nand_block_command_uops,                  {NAND_TIME_TWB_US, 0U, NAND_TIME_TWH_US, NAND_TIME_TR_US}},
        [OP_BLOCK_LOCK_TIGHT]               = {              OP_BLOCK_LOCK_TIGHT,               "BLOCK_LOCK_TIGHT",     NAND_CMD_BLOCK_LOCK_TIGHT,                            0U, 0U, 0U, 0U, 1U,        g_nand_block_command_uops,                  {NAND_TIME_TWB_US, 0U, NAND_TIME_TWH_US, NAND_TIME_TR_US}},
        [OP_BLOCK_LOCK_STATUS]              = {             OP_BLOCK_LOCK_STATUS,              "BLOCK_LOCK_STATUS",    NAND_CMD_BLOCK_LOCK_STATUS,                            0U, 3U, 0U, 0U, 1U,    g_nand_block_lock_status_uops,                  {NAND_TIME_TWB_US, 0U, NAND_TIME_TWH_US, NAND_TIME_TR_US}},
        [OP_OTP_DATA_LOCK]                  = {                 OP_OTP_DATA_LOCK,                  "OTP_DATA_LOCK",   NAND_CMD_PROGRAM_PAGE_SETUP, NAND_CMD_PROGRAM_PAGE_CONFIRM, 5U, 0U, 0U, 0U,             g_nand_otp_lock_uops,               {NAND_TIME_TWB_US, 0U, NAND_TIME_TWH_US, NAND_TIME_TPROG_US}},
        [OP_OTP_DATA_PROGRAM]               = {              OP_OTP_DATA_PROGRAM,               "OTP_DATA_PROGRAM",   NAND_CMD_PROGRAM_PAGE_SETUP, NAND_CMD_PROGRAM_PAGE_CONFIRM, 5U, 1U, 0U, 0U,  g_nand_normal_program_page_uops,               {NAND_TIME_TWB_US, 0U, NAND_TIME_TWH_US, NAND_TIME_TPROG_US}},
        [OP_OTP_DATA_READ]                  = {                 OP_OTP_DATA_READ,                  "OTP_DATA_READ",      NAND_CMD_READ_PAGE_SETUP,    NAND_CMD_READ_PAGE_CONFIRM, 5U, 0U, 0U, 0U,     g_nand_normal_read_page_uops,                  {NAND_TIME_TWB_US, 0U, NAND_TIME_TWH_US, NAND_TIME_TR_US}},
};

uint8_t nand_operation_has_data(uint8_t operation)
{
    if (operation >= OP_COUNT)
    {
        return 0U;
    }
    if (g_nand_operation_map[operation].data_input_cycles > 0U)
    {
        return 1U;
    }
    if ((operation == OP_RESET) || (operation == OP_READ_MODE) ||
        (operation == OP_BLOCK_LOCK) || (operation == OP_BLOCK_LOCK_TIGHT) ||
        (operation == OP_BLOCK_UNLOCK_LOW) || (operation == OP_BLOCK_UNLOCK_HIGH) ||
        (operation == OP_READ_FOR_INTERNAL_DATA_MOVE) || (operation == OP_OTP_DATA_LOCK))
    {
        return 0U;
    }
    if (operation == OP_ERASE_BLOCK)
    {
        return 0U;
    }
    return 1U;
}

uint16_t nand_operation_data_length(uint8_t operation)
{
    switch (operation)
    {
        case OP_READ_ID:
            return 5U;
        case OP_READ_PARAMETER_PAGE:
        case OP_READ_UNIQUE_ID:
            return 256U;
        case OP_GET_FEATURES:
        case OP_SET_FEATURES:
            return 4U;
        case OP_READ_STATUS:
        case OP_READ_STATUS_ENHANCED:
        case OP_BLOCK_LOCK_STATUS:
            return 1U;
        case OP_RANDOM_DATA_READ:
        case OP_RANDOM_DATA_INPUT:
            return PAGE_SIZE / CW_NUM;
        case OP_PROGRAM_FOR_INTERNAL_DATA_MOVE:
        case OP_PROGRAM_FOR_INTERNAL_MOVE:
            return 1U;
        case OP_NORMAL_READ_PAGE:
        case OP_NORMAL_PROGRAM_PAGE:
        case OP_PROGRAM_PAGE_CACHE:
        case OP_READ_PAGE_CACHE_SEQ:
        case OP_READ_PAGE_CACHE_RANDOM:
        case OP_READ_PAGE_CACHE_LAST:
        case OP_OTP_DATA_PROGRAM:
        case OP_OTP_DATA_READ:
            return PAGE_SIZE;
        case OP_RESET:
        case OP_READ_MODE:
        case OP_BLOCK_LOCK:
        case OP_BLOCK_LOCK_TIGHT:
        case OP_BLOCK_UNLOCK_LOW:
        case OP_BLOCK_UNLOCK_HIGH:
        case OP_READ_FOR_INTERNAL_DATA_MOVE:
        case OP_OTP_DATA_LOCK:
        case OP_ERASE_BLOCK:
        default:
            return 0U;
    }
}
