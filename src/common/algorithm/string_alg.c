
#include <common.h>


char * longestCommonPrefix( char * *strs, int strsSize )
{
	/*

		if (strsSize == 0 || strs[0] == NULL)
			{
		return strdup("");
			}
		char* str = strs[0];
		for (int i = 0; str[i]; i++) {
			for (int j = 1; j < strsSize; j++) {
				if (strs[j][i] == '\0' || strs[j][i] != str[i]) {
					char* result = malloc(i + 1);
					strncpy(result, str, i);
					result[i] = '\0';
					return result;
				}
			}
		}
		return strdup(str);
		*/
	if ( strsSize == 0 || strs == NULL || strs[0] == NULL )
	{
			char *			result = malloc( 1 );

			if ( result )
				result[0] = '\0';

			return result;
	}

	char *			str = strs[0];
	int 			len = 0;

	// 逐个字符检查
	while ( str[len] != '\0' )
	{
			for ( int i = 1; i < strsSize; i++ )
			{
					if ( strs[i] == NULL || strs[i][len] != str[len] )
					{
							// 发现不同，截断并返回
							char *			result = malloc( len + 1 );

							if ( result == NULL )
								return NULL;

							strncpy( result, str, len );
							result[len] 		= '\0';
							return result;
					}
			}

			len++;
	}

	// 所有字符串都相同
	char *			result = malloc( len + 1 );

	if ( result == NULL )
		return NULL;

	strcpy( result, str );
	return result;
}


int romanToInt( char * s )
{
	int 			symbol_val[26] =
	{
			0
	};
	symbol_val['I' - 'A'] = 1;
	symbol_val['V' - 'A'] = 5;
	symbol_val['X' - 'A'] = 10;
	symbol_val['L' - 'A'] = 50;
	symbol_val['C' - 'A'] = 100;
	symbol_val['D' - 'A'] = 500;
	symbol_val['M' - 'A'] = 1000;

	int 			len = strlen( s );
	int 			total = 0;

	for ( int i = 0; i < len; i++ )
	{
			int 			val = symbol_val[s[i] - 'A'];

			if ( i < len - 1 && val < symbol_val[s[i + 1] - 'A'] )
			{
					total				-= val;
			}
			else 
			{
					total				+= val;
			}
	}

	return total;
}



char * intToRoman( int num )
{
	static const int values[] =
	{
			1000, 900, 500, 400, 100, 90, 50, 40, 10, 9, 5, 4, 1
	};
	static const char * symbols[] =
	{
			"M", "CM", "D", "CD", "C", "XC", "L", "XL", "X", "IX", "V", "IV", "I"
	};

	char *			roman = malloc( sizeof( char ) * 16 );

	roman[0]			= '\0';

	for ( int i = 0; i < 13; i++ )
	{
			while ( num >= values[i] )
			{
					num 				-= values[i];
					strcpy( roman + strlen( roman ), symbols[i] );
			}

			if ( num == 0 )
			{
					break;
			}
	}

	return roman;
}


int lengthOfLastWord( char * s )
{
	int 			len = strlen( s );
	int 			i	= len - 1;

	// 跳过末尾空格
	while ( i >= 0 && s[i] == ' ' )
		i--;

	// 记录单词末尾
	int 			right = i;

	// 向前找空格
	while ( i >= 0 && s[i] != ' ' )
		i--;

	return right - i;
}


void reverse_c(char * string, int left, int right)
{
		while(left<right)
		{
				char tmp=string[left];
				string[left]=string[right];
				string[right]=tmp;
				left++;
				right--;
		}
}


char* reverseWords(char* s) {
 	   int len=strlen(s);
 	   
		 int slow=0;
 	   for(int fast=0;fast<len;fast++)
 	   {
					if(s[fast]==' ')
					{
							continue;
					}

					if(slow>0)
					{
						s[slow++]=' ';
					}

					while(fast<len&&s[fast]!=' ')
					{
						s[slow++]=s[fast++];
					}
 	   }

 	   s[slow]='\0';
 	   reverse_c(s,0,slow-1);

 	   int start=0;
 	   for(int i=0;i<=slow;i++)
 	   {
				if(s[i]==' '||s[i]=='\0')
				{
						reverse_c(s,start,i-1);
						start=i+1;
				}
 	   }
 	   return s;
}

char* convert(char* s, int numRows) {
    int len = strlen(s);
    if (numRows == 1 || len <= numRows) {
        // 特殊情况：只有一行，或字符串长度小于行数
        char* result = (char*)malloc((len + 1) * sizeof(char));
        strcpy(result, s);
        return result;
    }

    char* result = (char*)malloc((len + 1) * sizeof(char));
    int idx = 0;
    int cycle = 2 * numRows - 2;  // 一个完整的 V 形周期

    for (int row = 0; row < numRows; row++) {
        for (int j = row; j < len; j += cycle) {
            // 当前周期的第一个字符
            result[idx++] = s[j];
            
            // 如果不是第一行和最后一行，中间还有字符
            int mid = j + cycle - 2 * row;
            if (row > 0 && row < numRows - 1 && mid < len) {
                result[idx++] = s[mid];
            }
        }
    }
    result[idx] = '\0';
    return result;
}


void computeLPS(char* pattern, int m, int* lps) {
    int len = 0;    // 当前最长相等前后缀的长度
    lps[0] = 0;     // 第一个字符的 LPS 总是 0
    int i = 1;
    
    while (i < m) {
        if (pattern[i] == pattern[len]) {
            // 当前字符和前缀的下一个字符相等
            len++;
            lps[i] = len;
            i++;
        } else {
            if (len != 0) {
                // 回退到上一个 LPS
                len = lps[len - 1];
            } else {
                // 没有相等的前后缀
                lps[i] = 0;
                i++;
            }
        }
    }
}

int strStr(char* haystack, char* needle) {
    int n = strlen(haystack);
    int m = strlen(needle);
    
    if (m == 0) return 0;
    
    // 计算前缀函数
    int* lps = (int*)malloc(m * sizeof(int));
    computeLPS(needle, m, lps);
    
    int i = 0;  // haystack 的索引
    int j = 0;  // needle 的索引
    
    while (i < n) {
        if (haystack[i] == needle[j]) {
            i++;
            j++;
        }
        
        if (j == m) {
            free(lps);
            return i - j;  // 找到匹配
        } else if (i < n && haystack[i] != needle[j]) {
            if (j != 0) {
                j = lps[j - 1];
            } else {
                i++;
            }
        }
    }
    
    free(lps);
    return -1;
}


