
#include <common.h>

/*
char* addBinary(char* a, char* b) {
	int len_a = strlen(a);
	int len_b = strlen(b);
	int max_len = (len_a > len_b) ? len_a : len_b;

	char* ret = (char*)malloc(sizeof(char) * (max_len + 2));
	if (ret == NULL) return NULL;

	int idx = max_len + 1;
	ret[idx] = '\0';

	int carry = 0;
	int i = len_a - 1;
	int j = len_b - 1;

	while (i >= 0 || j >= 0 || carry > 0) {
		if (i >= 0) {
			carry += a[i--] - '0';
		}
		if (j >= 0) {
			carry += b[j--] - '0';
		}

		// ✅ 修正：取最低位作为当前位
		ret[--idx] = (carry & 1) + '0';
		carry >>= 1;  // 右移一位，去掉最低位
	}

	// ✅ 修正：将结果移到开头，避免返回 ret + idx 的释放问题
	if (idx > 0) {
		int shift = idx;
		int k = 0;
		while (ret[shift] != '\0') {
			ret[k++] = ret[shift++];
		}
		ret[k] = '\0';
	}

	return ret;
}
*/
//前向遍历的分支预测要准确一点
char * addBinary(char * a, char * b)
{
	// 1. 反转字符串 a（内联）
	int 			len_a = strlen(a);

	for (int i = 0; i < len_a / 2; i++) {
		char			temp = a[i];

		a[i]				= a[len_a - i - 1];
		a[len_a - i - 1]	= temp;
	}

	// 2. 反转字符串 b（内联）
	int 			len_b = strlen(b);

	for (int i = 0; i < len_b / 2; i++) {
		char			temp = b[i];

		b[i]				= b[len_b - i - 1];
		b[len_b - i - 1]	= temp;
	}

	// 3. 加法
	int 			max_len = (len_a > len_b) ? len_a: len_b;
	char *			result = (char *) malloc(max_len + 2);

	if (result == NULL)
		return NULL;

	int 			carry = 0;
	int 			idx = 0;

	for (int i = 0; i < max_len; i++) {
		if (i < len_a)
			carry += a[i] - '0';

		if (i < len_b)
			carry += b[i] - '0';

		result[idx++]		= (carry & 1) + '0';
		carry >>= 1;
	}

	if (carry) {
		result[idx++]		= '1';
	}

	result[idx] 		= '\0';

	// 4. 反转结果字符串（内联）
	for (int i = 0; i < idx / 2; i++) {
		char			temp = result[i];

		result[i]			= result[idx - i - 1];
		result[idx - i - 1] = temp;
	}

	return result;
}


uint32_t reverseBits(uint32_t n)
{
	// 步骤1：交换相邻位 (1位一组 → 2位一组)
	n					= ((n & 0x55555555) << 1) | ((n & 0xAAAAAAAA) >> 1);

	// 0x55555555 = 01010101010101010101010101010101
	// 0xAAAAAAAA = 10101010101010101010101010101010
	// 步骤2：交换2位组 (2位一组 → 4位一组)
	n					= ((n & 0x33333333) << 2) | ((n & 0xCCCCCCCC) >> 2);

	// 0x33333333 = 00110011001100110011001100110011
	// 0xCCCCCCCC = 11001100110011001100110011001100
	// 步骤3：交换4位组 (4位一组 → 8位一组)
	n					= ((n & 0x0F0F0F0F) << 4) | ((n & 0xF0F0F0F0) >> 4);

	// 0x0F0F0F0F = 00001111000011110000111100001111
	// 0xF0F0F0F0 = 11110000111100001111000011110000
	// 步骤4：交换8位组 (8位一组 → 16位一组)
	n					= ((n & 0x00FF00FF) << 8) | ((n & 0xFF00FF00) >> 8);

	// 0x00FF00FF = 00000000111111110000000011111111
	// 0xFF00FF00 = 11111111000000001111111100000000
	// 步骤5：交换16位组 (16位一组 → 32位一组)
	n					= (n << 16) | (n >> 16);

	return n;
}


