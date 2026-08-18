
#include <common.h>

char *longestCommonPrefix(char **strs, int strsSize)
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
    if (strsSize == 0 || strs == NULL || strs[0] == NULL)
    {
        char *result = malloc(1);

        if (result)
            result[0] = '\0';

        return result;
    }

    char *str = strs[0];
    int   len = 0;

    // 逐个字符检查
    while (str[len] != '\0')
    {
        for (int i = 1; i < strsSize; i++)
        {
            if (strs[i] == NULL || strs[i][len] != str[len])
            {
                // 发现不同，截断并返回
                char *result = malloc(len + 1);

                if (result == NULL)
                    return NULL;

                strncpy(result, str, len);
                result[len] = '\0';
                return result;
            }
        }

        len++;
    }

    // 所有字符串都相同
    char *result = malloc(len + 1);

    if (result == NULL)
        return NULL;

    strcpy(result, str);
    return result;
}
