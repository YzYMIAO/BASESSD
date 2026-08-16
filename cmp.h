

#ifndef MUNIT_H_
#define MUNIT_H_
#include <inttypes.h>	// 1. 修正：闭合尖括号
#include <stdbool.h>	// 如果使用 bool，需要包含此头文件
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>

#define MUNIT_ENABLE_ASSERT_ALIASES

/**************************************************************************
*							 断言宏定义
***************************************************************************/
typedef enum Munit_LOG_Level_e {
	MUNIT_LOG_DEBUG, 
	MUNIT_LOG_INFO, 
	MUNIT_LOG_WARNING, 
	MUNIT_LOG_ERROR
} MunitLogLevel;


#if defined(__GNUC__)			&& !defined(__MINGW32__)
#define MUNIT_PRINTF(string_index, first_to_check)						\
		__attribute__((format(printf, string_index, first_to_check)))

#else

#define MUNIT_PRINTF(string_index, first_to_check) 
#endif

MUNIT_PRINTF(5, 0) // 2. 修正：MUNIT_PRINFT → MUNIT_PRINTF
void munit_logf_ex(MunitLogLevel level, const char * filename, int line, const char * format, ...);

#define munit_logf(level, format, ...)									\
		munit_logf_ex(level, __FILE__, __LINE__, format, __VA_ARGS__)

#define munit_log(level, msg)											\
		munit_logf(level, "%s", msg)

MUNIT_PRINTF(3, 4) void munit_errorf_ex(const char * filename, int line, const char * format, ...);

#define munit_errorf(format, ...)										\
		munit_errorf_ex(__FILE__, __LINE__, format, __VA_ARGS__)

#define munit_error(msg)												\
		munit_errorf("%s", msg)

#if defined(__GNUC__)			
#define MUNIT_LIKELY(expr)		(__builtin_expect((expr), 1))	// 3. 修正：括号位置
#define MUNIT_UNLIKELY(expr)	(__builtin_expect((expr), 0))
#define MUNIT_UNUSED			__attribute__((__unused__)) 	// 4. 修正：添加双下划线

#else

#define MUNIT_LIKELY(expr)		(expr)
#define MUNIT_UNLIKELY(expr)	(expr)
#define MUNIT_UNUSED
#endif

#define munit_assert(expr)												\
		do {															\
		if (!MUNIT_LIKELY(expr)) {										\
		munit_error("assertion failed: " #expr);						\
		}																\
		} while (0)

#define munit_assert_true(expr) \	// 5. 修正：ture → true

do 
{
	\

	if (!MUNIT_LIKELY(expr)) {
		\ munit_error("assertion failed: "
#expr " is not true"					);								\
				}														\
				} while (0)

#define 		munit_assert_false(expr)								\
				do {													\
				if (!MUNIT_LIKELY(!(expr))) { \   // 6. 修正：添加括号

		munit_error("assertion failed: "
#expr " is not false"					);								\
				}														\
				} while (0)

#define 		munit_assert_type_full(prefix, suffix, T, fmt, a, op, b) \
				do {													\
				T munit_tmp_a_ = (a);									\
				T munit_tmp_b_ = (b);									\
				if (!(munit_tmp_a_ op munit_tmp_b_)) {					\
				munit_errorf("assertion failed: %s %s %s (" prefix "%" fmt suffix " %s " prefix "%" fmt suffix ")", \
				#a, #op, #b, munit_tmp_a_, #op, munit_tmp_b_); \   // 7. 修正：逗号位置
	}

	\
}


while(0)

#define munit_assert_type(T, fmt, a, op, b) munit_assert_type_full("", "", T, fmt, a, op, b)
#define munit_assert_char(a, op, b) munit_assert_type_full("'\\x", "'", char, "02hhx", a, op, b)
#define munit_assert_uchar(a, op, b) munit_assert_type_full("'\\x", "'", unsigned char, "02hhx", a, op, b)
#define munit_assert_short(a, op, b) munit_assert_type(short, "hd", a, op, b)
#define munit_assert_ushort(a, op, b) munit_assert_type(unsigned short, "hu", a, op, b)
#define munit_assert_int(a, op, b) munit_assert_type(int, "d", a, op, b)
#define munit_assert_uint(a, op, b) munit_assert_type(unsigned int, "u", a, op, b)
#define munit_assert_long(a, op, b) munit_assert_type(long int, "ld", a, op, b)
#define munit_assert_ulong(a, op, b) munit_assert_type(unsigned long int, "lu", a, op, b)
#define munit_assert_llong(a, op, b) munit_assert_type(long long int, "lld", a, op, b)
#define munit_assert_ullong(a, op, b) munit_assert_type(unsigned long long int, "llu", a, op, b)
#define munit_assert_size(a, op, b) munit_assert_type(size_t, "zu", a, op, b)
#define munit_assert_float(a, op, b) munit_assert_type(float, "f", a, op, b)
#define munit_assert_double(a, op, b) munit_assert_type(double, "g", a, op, b)
#define munit_assert_ptr(a, op, b) munit_assert_type(const void*, "p", a, op, b)
#define munit_assert_int8(a, op, b) munit_assert_type(int8_t, PRIi8, a, op, b)
#define munit_assert_uint8(a, op, b) munit_assert_type(uint8_t, PRIu8, a, op, b)
#define munit_assert_int16(a, op, b) munit_assert_type(int16_t, PRIi16, a, op, b)
#define munit_assert_uint16(a, op, b) munit_assert_type(uint16_t, PRIu16, a, op, b)
#define munit_assert_int32(a, op, b) munit_assert_type(int32_t, PRIi32, a, op, b)
#define munit_assert_uint32(a, op, b) munit_assert_type(uint32_t, PRIu32, a, op, b)
#define munit_assert_int64(a, op, b) munit_assert_type(int64_t, PRIi64, a, op, b)
#define munit_assert_uint64(a, op, b) munit_assert_type(uint64_t, PRIu64, a, op, b)

#define munit_assert_double_equal(a, b, precision)						\
		do {															\
		const double munit_tmp_a_ = (a);								\
		const double munit_tmp_b_ = (b);								\
		const double munit_tmp_diff = ((munit_tmp_a_ - munit_tmp_b_) < 0) ? \	// 10. 修正：munit_tmo_a_ → munit_tmp_a_

- (munit_tmp_a_ - munit_tmp_b_): (munit_tmp_a_ - munit_tmp_b_);
\

if (MUNIT_UNLIKELY(munit_tmp_diff > 1e -
## precision					)) { \	 // 11. 修正：LIKELY → UNLIKELY
munit_errorf("assertion failed: %s == %s (%0."
#precision "g == %0."			#precision "g)",						\
		#a, #b, munit_tmp_a_, munit_tmp_b_);							\
		}																\
		} while (0)

#define munit_assert_string_equal(a, b) 								\
		do {															\
		const char* munit_tmp_a_ = (a); 								\
		const char* munit_tmp_b_ = (b); 								\
		if (MUNIT_UNLIKELY(strcmp(munit_tmp_a_, munit_tmp_b_) != 0)) { \   // 12. 修正：变量名

munit_errorf("assertion failed: string %s == %s (\"%s\" == \"%s\")", \
#a ,							#b, munit_tmp_a_, munit_tmp_b_);		\
		}																\
		} while (0)

#define munit_assert_string_not_equal(a, b) 							\
		do {															\
		const char* munit_tmp_a_ = (a); 								\
		const char* munit_tmp_b_ = (b); 								\
		if (MUNIT_UNLIKELY(strcmp(munit_tmp_a_, munit_tmp_b_) == 0)) {	\
		munit_errorf("assertion failed: string %s != %s (\"%s\" == \"%s\")", \
		#a, #b, munit_tmp_a_, munit_tmp_b_);							\
		}																\
		} while (0)

#define munit_assert_memory_equal(size, a, b)							\
		do {															\
		const unsigned char* munit_tmp_a_ = (const unsigned char*)(a);	\
		const unsigned char* munit_tmp_b_ = (const unsigned char*)(b);	\
		const size_t munit_tmp_size = (size);							\
		if (MUNIT_UNLIKELY(memcmp(munit_tmp_a_, munit_tmp_b_, munit_tmp_size) != 0)) { \   // 13. 修正：添加长度参数

size_t munit_tmp_pos; \ for(munit_tmp_pos = 0; munit_tmp_pos < munit_tmp_size;
	 munit_tmp_pos++) { \ if(munit_tmp_a_[munit_tmp_pos] != munit_tmp_b_[munit_tmp_pos]) { \ munit_errorf("assertion failed: memory %s == %s, at offset %zu", \ // 14. 修正：变量名
#a ,							#b, munit_tmp_pos); 					\
		break;															\
		}																\
		}																\
		}																\
		} while (0)

#define munit_assert_memory_not_equal(size, a, b)						\
		do {															\
		const unsigned char* munit_tmp_a_ = (const unsigned char*)(a);	\
		const unsigned char* munit_tmp_b_ = (const unsigned char*)(b);	\
		const size_t munit_tmp_size = (size);							\
		if (MUNIT_UNLIKELY(memcmp(munit_tmp_a_, munit_tmp_b_, munit_tmp_size) == 0)) { \
		munit_errorf("assertion failed: memory %s != %s (%zu bytes)",	\
		#a, #b, munit_tmp_size);										\
		}																\
		} while (0)

#define munit_assert_ptr_equal(a, b)									\
		munit_assert_ptr(a, ==, b)

#define munit_assert_ptr_not_equal(a, b)								\
		munit_assert_ptr(a, !=, b)

#define munit_assert_null(ptr)											\
		munit_assert_ptr(ptr, ==, NULL)

#define munit_assert_not_null(ptr)										\
		munit_assert_ptr(ptr, !=, NULL)

#define munit_assert_ptr_null(ptr)										\
		munit_assert_ptr(ptr, ==, NULL)

#define munit_assert_ptr_not_null(ptr)									\
		munit_assert_ptr(ptr, !=, NULL)

/*** 内存分配 ***/
void * munit_malloc_ex(const char * filename, int line, size_t size);
#define munit_malloc(size)												\
		munit_malloc_ex(__FILE__, __LINE__, (size))

#define munit_new(type) 												\
		((type*)munit_malloc(sizeof(type)))

#define munit_calloc(nmemb, size)										\
		munit_malloc((nmemb) * (size))

#define munit_newa(type, nmemb) 										\
		((type*)munit_calloc(nmemb, sizeof(type)))

#if defined(MUNIT_ENABLE_ASSERT_ALIASES) 
#define assert_true(expr)		munit_assert_true(expr)
#define assert_false(expr)		munit_assert_false(expr)
#define assert_char(a, op, b)	munit_assert_char(a, op, b)
#define assert_uchar(a, op, b)	munit_assert_uchar(a, op, b)
#define assert_short(a, op, b)	munit_assert_short(a, op, b)
#define assert_ushort(a, op, b) munit_assert_ushort(a, op, b)
#define assert_int(a, op, b)	munit_assert_int(a, op, b)
#define assert_uint(a, op, b)	munit_assert_uint(a, op, b)
#define assert_long(a, op, b)	munit_assert_long(a, op, b)
#define assert_ulong(a, op, b)	munit_assert_ulong(a, op, b)
#define assert_llong(a, op, b)	munit_assert_llong(a, op, b)
#define assert_ullong(a, op, b) munit_assert_ullong(a, op, b)
#define assert_size(a, op, b)	munit_assert_size(a, op, b)
#define assert_float(a, op, b)	munit_assert_float(a, op, b)
#define assert_double(a, op, b) munit_assert_double(a, op, b)
#define assert_ptr(a, op, b)	munit_assert_ptr(a, op, b)
#define assert_int8(a, op, b)	munit_assert_int8(a, op, b)
#define assert_uint8(a, op, b)	munit_assert_uint8(a, op, b)
#define assert_int16(a, op, b)	munit_assert_int16(a, op, b)
#define assert_uint16(a, op, b) munit_assert_uint16(a, op, b)
#define assert_int32(a, op, b)	munit_assert_int32(a, op, b)
#define assert_uint32(a, op, b) munit_assert_uint32(a, op, b)
#define assert_int64(a, op, b)	munit_assert_int64(a, op, b)
#define assert_uint64(a, op, b) munit_assert_uint64(a, op, b)
#define assert_double_equal(a, b, precision) munit_assert_double_equal(a, b, precision)
#define assert_string_equal(a, b) munit_assert_string_equal(a, b)
#define assert_string_not_equal(a, b) munit_assert_string_not_equal(a, b)
#define assert_memory_equal(size, a, b) munit_assert_memory_equal(size, a, b)
#define assert_memory_not_equal(size, a, b) munit_assert_memory_not_equal(size, a, b)
#define assert_ptr_equal(a, b)	munit_assert_ptr_equal(a, b)
#define assert_ptr_not_equal(a, b) munit_assert_ptr_not_equal(a, b)
#define assert_ptr_null(ptr)	munit_assert_ptr_null(ptr)
#define assert_ptr_not_null(ptr) munit_assert_ptr_not_null(ptr)
#define assert_null(ptr)		munit_assert_null(ptr)
#define assert_not_null(ptr)	munit_assert_not_null(ptr)
#endif

#if defined(MUNIT_ENABLE_ASSERT_ALIASES) 

#if defined(assert) 			
#undef assert
#endif

#define assert(expr)			munit_assert(expr)
#endif

/**************************************************************************
*							  核心上下文结构体定义
***************************************************************************/
typedef enum Munit_Result_e { MUNIT_OK, 
	MUNIT_FAIL, 
	MUNIT_SKIP, 
	MUNIT_ERROR } MunitResult; typedef struct Munit_Parameter_s { char * name; char * value; } MunitParameter;
	 typedef MunitResult(*MunitTestFunc) (const MunitParameter params[], void * user_data_or_fixture);
	 typedef void * (* MunitTestSetup) (const MunitParameter params[], void * user_data);
	 typedef void(*MunitTestTearDown) (void * fixture);
	 typedef enum Munit_Test_Options_s { MUNIT_TEST_OPTION_NONE = 0, 
	MUNIT_TEST_OPTION_SINGLE_ITERATION = 1 << 0, 
	MUNIT_TEST_OPTION_TODO = 1 << 1 } MunitTestOptions; typedef struct Munit_Parameter_Enum_s { char * name;
	 char * *values; } MunitParameterEnum; typedef struct Munit_Test_s { char * name; MunitTestFunc test;
	 MunitTestSetup setup; MunitTestTearDown tear_down; MunitTestOptions option; MunitParameterEnum * parameters;
	 } MunitTest; typedef enum Munit_Suite_Option_e { MUNIT_SUITE_OPTION_NONE = 0 } MunitSuiteOption;
	 typedef struct Munit_Suite_s { char * prefix; MunitTest * tests; MunitSuite * suites; // 15. 修正：suite → suites（复数）
unsigned int iterations; MunitSuiteOption options; } MunitSuite; typedef struct Munit_Argument_s { char * name;
	 munit_bool (*parse_argument) (const MunitSuite * suite, void * user_data, int * arg, int argc, char * const argv[]);
	 // 16. 修正：bool → munit_bool
void(*write_help) (const MunitArgument * argument, void * user_data); } MunitArgument;
#endif /* MUNIT_H_ */

