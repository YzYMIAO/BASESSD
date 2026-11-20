/* ============================================================================
 * 热代码段（产品与 UT 共用）
 *
 * 由 stm32h743xx_flash.ld 与 stm32h743xx_flash_ut.ld 通过 INCLUDE 共同使用。
 * 过去两份脚本各自抄一份 KEEP 清单；UT 脚本照抄产品脚本时，release 选择条件
 * `*release/obj/` 与 UT 的对象目录 `release/obj_ut_*` 不符，导致 UT release
 * 的放置块永远不命中（RISK-BUILD-04）。共享后只维护一份。
 *
 * 选择条件是「输入对象路径」：
 *   - 公共集：debug / release、产品 / UT 都命中；
 *   - release 追加集：路径含 `release/obj/`（产品）或 `release/obj_ut_*`（UT），
 *     所以只有 release 档命中；debug 的对象目录不含 `release/`，不命中。
 *   这不是 C 预处理开关，是链接期按对象路径选择。
 *
 * 容量约束：公共集必须装得进 -O0 debug 的 64 KB ITCM；release 追加集也必须在
 * 64 KB 内。两档都由链接期 ASSERT 兜底（见各主脚本）。
 *
 * 启动代码按 __ssd_code_high_start / __ssd_code_high_end / load_start 把本段
 * 从 Flash 复制到 ITCM 执行。
 *
 * ⚠ 注释里不要写「星号紧跟斜杠」的两字符序列（包括放在反引号或示例里）：
 *   它会提前闭合注释，后面的中文会被 ld 当成脚本语法解析。
 * ========================================================================== */

  .ssd_code_high :
  {
    . = ALIGN(4);
    __ssd_code_high_start = .;
    /* 公共集：host 数据面（FTL submit/complete、L2P、wbuf/pending、cache
     * read/write）、FC pump/BCH/池/加扰/ECC 包装、NAND dispatch/FMC。
     * CRC 引擎（小、调用点少）留 Flash，给 UT 构建留 ITCM 余量。 */
    KEEP(*ftl/ftl.o(.text .text.*))
    KEEP(*ftl/l2p_cache.o(.text .text.*))
    KEEP(*ftl/wbuf.o(.text .text.*))
    KEEP(*ftl/pending.o(.text .text.*))
    KEEP(*ftl/cache_batch.o(.text .text.*))
    KEEP(*fc/fc.o(.text .text.*))
    KEEP(*fc/fc_data_path.o(.text .text.*))
    KEEP(*fc/fc_pool.o(.text .text.*))
    KEEP(*fc/fc_random_engine.o(.text .text.*))
    KEEP(*fc/fc_ecc_engine.o(.text .text.*))
    KEEP(*fc/fc_bch_engine.o(.text .text.*))
    KEEP(*nand/nand_dispatch.o(.text .text.*))
    KEEP(*nand/nand_fmc.o(.text .text.*))
    /* release 追加集：写/GC/后台路径。对象路径里 obj 后跟任意字符，因此同时
     * 匹配产品的 obj/ 与 UT 的 obj_ut_*；debug 的对象目录不含 release/，不命中。 */
    KEEP(*release/obj*/ftl/allocator.o(.text .text.*))
    KEEP(*release/obj*/ftl/journal.o(.text .text.*))
    KEEP(*release/obj*/ftl/metadata.o(.text .text.*))
    KEEP(*release/obj*/ftl/metadata_job.o(.text .text.*))
    KEEP(*release/obj*/ftl/migrate.o(.text .text.*))
    KEEP(*release/obj*/ftl/gc.o(.text .text.*))
    /* wear_leveling / bg_scheduler 是纯后台路径，放 Flash 运行；写回 CMT 让
     * l2p_cache.o 增大后，ITCM 优先留给 host 数据面。 */
    KEEP(*release/obj*/ftl/ftl_op.o(.text .text.*))
    KEEP(*release/obj*/fc/fc_interface.o(.text .text.*))
    KEEP(*release/obj*/fc/fc_queue.o(.text .text.*))
    KEEP(*release/obj*/fc/fc_command.o(.text .text.*))
    KEEP(*release/obj*/nand/nand_engine.o(.text .text.*))
    KEEP(*(.ssd_code_high))
    KEEP(*(.ssd_code_high.*))
    . = ALIGN(4);
    __ssd_code_high_end = .;
  } > ITCM AT > FLASH
  __ssd_code_high_load_start = LOADADDR(.ssd_code_high);

  .ssd_code_fast :
  {
    . = ALIGN(4);
    KEEP(*(.ssd_code_fast))
    KEEP(*(.ssd_code_fast.*))
    . = ALIGN(4);
  } > FLASH
