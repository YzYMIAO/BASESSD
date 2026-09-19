
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// 交换两块内存
static void swap(void *a, void *b, size_t size)
{
    char *pa = (char *)a;
    char *pb = (char *)b;

    for (size_t i = 0; i < size; i++)
    {
        char tmp = pa[i];

        pa[i] = pb[i];
        pb[i] = tmp;
    }
}

/**
 * 函数名：bubbleSort
 * 功能：对任意类型的数组进行冒泡排序,适合小规模数据
 * 参数：
 *		base	- 待排序数组的首地址
 *		nmemb	- 数组中元素的个数
 *		size	- 每个元素的大小（字节数）
 *		compar	- 比较函数的指针，用于比较两个元素
 * 返回值：无
 * 注意：排序结果是升序，比较函数需要遵循qsort的约定
 * 原理：重复遍历数组，比较相邻元素，顺序错误就交换。
 */
void bubble_sort(void *base, size_t num, size_t size, int (*compar)(const void *, const void *))
{
    if (base == NULL || num == 0 || size == 0 || compar == NULL)
    {
        return;
    }

    char *arr = (char *)base;

    for (size_t i = 0; i < num - 1; i++)
    {
        size_t swapped = 0;

        for (size_t j = 0; j < num - 1 - i; j++)
        {
            void *elem1 = arr + j * size;
            void *elem2 = arr + (j + 1) * size;

            if (compar(elem1, elem2) > 0)
            {
                swap(elem1, elem2, size);
                swapped = 1;
            }
        }

        if (!swapped)
        {
            break;
        }
    }
}

/**
 * selection_sort
 * 功能：对任意类型的数组进行选择排序
 * 参数：
 *		base	- 待排序数组的首地址
 *		nmemb	- 数组中元素的个数
 *		size	- 每个元素的大小（字节数）
 *		compar	- 比较函数的指针，用于比较两个元素
 * 返回值：无
 * 注意：选择结果是升序，比较函数需要遵循qsort的约定
 * 原理：每次从未排序部分选择最小元素，放到已排序部分的末尾
 */
void selection_sort(void *base, size_t num, size_t size, int (*compar)(const void *, const void *))
{
    if (base == NULL || num == 0 || size == 0 || compar == NULL)
    {
        return; // 参数检查
    }

    char *arr = (char *)base;

    for (size_t i = 0; i < num - 1; i++)
    {
        size_t min_index = i;

        for (size_t j = i + 1; j < num; j++)
        {
            void *elem_j   = arr + j * size;
            void *elem_min = arr + min_index * size;

            if (compar(elem_j, elem_min) < 0)
            {
                min_index = j;
            }
        }

        if (min_index != i)
        {
            void *elem_i   = arr + i * size;
            void *elem_min = arr + min_index * size;

            swap(elem_i, elem_min, size);
        }
    }
}

/**
 * selection_sort
 * 功能：对任意类型的数组进行插入排序，适合小规模或基本有序的数据
 * 参数：
 *		base	- 待排序数组的首地址
 *		nmemb	- 数组中元素的个数
 *		size	- 每个元素的大小（字节数）
 *		compar	- 比较函数的指针，用于比较两个元素
 * 返回值：无
 * 注意：选择结果是升序，比较函数需要遵循qsort的约定
 * 原理：将未排序元素逐个插入到已排序部分的合适位置。
 */
/*
void insertion_sort(void * base, size_t num, size_t size, int(*compar) (const void *, const void *))
{
    if (base == NULL || num < 2 || size == 0 || compar == NULL)
        return;

    char *			arr = (char *) base;

    for (size_t i = 1; i < num; i++)
    {
        size_t			j	= i;

        // 从后往前比较，直到找到正确位置
        while (j > 0 && compar(arr + j * size, arr + (j - 1) *size) < 0)
        {
            //这一步发生的是交换，三次赋值，没有申请临时变量的一次赋值效率高
            swap(arr + j * size, arr + (j - 1) *size, size);
            j--;
        }
    }
}
*/
void insertion_sort(void *base, size_t num, size_t size, int (*compar)(const void *, const void *))
{
    if (base == NULL || num < 2 || size == 0 || compar == NULL)
    {
        return;
    }

    char *arr  = (char *)base;
    char *temp = malloc(size);

    if (temp == NULL)
    {
        return;
    }

    for (size_t i = 1; i < num; i++)
    {
        memcpy(temp, arr + i * size, size);
        size_t j = i;

        while (j > 0 && compar(temp, arr + (j - 1) * size) < 0)
        {
            memcpy(arr + j * size, arr + (j - 1) * size, size);
            j--;
        };

        memcpy(arr + j * size, temp, size);
    }

    free(temp);
}

/**
 * shell_sort
 * 功能：对任意类型的数组进行shell排序，中等规模数据表现好
 * 参数：
 *		base	- 待排序数组的首地址
 *		nmemb	- 数组中元素的个数
 *		size	- 每个元素的大小（字节数）
 *		compar	- 比较函数的指针，用于比较两个元素
 * 返回值：无
 * 注意：选择结果是升序，比较函数需要遵循qsort的约定，不稳定排序
 * 原理：将未排序元素逐个插入到已排序部分的合适位置，插入排序的改进版，通过增量?
    ??组进行排序，逐步缩小增量。
 */
void shell_sort(void *base, size_t num, size_t size, int (*compar)(const void *, const void *))
{
    if (base == NULL || num < 2 || size == 0 || compar == NULL)
    {
        return;
    }

    char *arr  = (char *)base;
    char *temp = malloc(size);

    for (size_t gap = num / 2; gap > 0; gap /= 2)
    {
        for (size_t i = gap; i < num; i++)
        {
            memcpy(temp, arr + i * size, size);
            size_t j;

            for (j = i; j >= gap && compar(arr + (j - gap) * size, temp) > 0; j -= gap)
            {
                memcpy(arr + j * size, arr + (j - gap) * size, size);
            }

            memcpy(arr + j * size, temp, size);
        }
    }

    free(temp);
}

// 取左中右三值中的中值放到最后并作为基准
static void *median_of_three(void *base, size_t num, size_t size, int (*compar)(const void *, const void *))
{
    char *arr = (char *)base;

    // 如果元素少于3个，直接返回第一个元素作为基准
    if (num < 3)
    {
        return arr;
    }

    // 取左中右三个位置
    char *p_left  = arr;
    char *p_mid   = arr + ((num - 1) / 2) * size;
    char *p_right = arr + (num - 1) * size;

    // 排序三个值（使 p_left <= p_mid <= p_right）
    if (compar(p_left, p_mid) > 0)
    {
        swap(p_left, p_mid, size);
    }

    if (compar(p_left, p_right) > 0)
    {
        swap(p_left, p_right, size);
    }

    if (compar(p_mid, p_right) > 0)
    {
        swap(p_mid, p_right, size);
    }

    // 将中值（p_mid）放到最后一个位置，作为基准
    swap(p_mid, p_right, size);

    return p_right;
}

// 分区函数，小于中值的在左边，执行完后大于中值的在右边，返回中值指针
//  分区函数（使用 < 而非 <=）
static void *partition(void *base, size_t num, size_t size, int (*compar)(const void *, const void *))
{
    char *arr = (char *)base;

    // 如果元素少于2个，无需分区
    if (num < 2)
    {
        return arr;
    }

    // 三数取中优化，基准值在最后一个位置
    void *pivot = median_of_three(base, num, size, compar);

    size_t i = 0;

    for (size_t j = 0; j < num - 1; j++)
    {
        char *cur = arr + j * size;

        // 关键：使用 < 而不是 <=，避免重复元素导致不平衡
        if (compar(cur, pivot) < 0)
        {
            swap(arr + i * size, cur, size);
            i++;
        }
    }

    // 将基准值放到正确位置（i 的位置）
    swap(arr + i * size, pivot, size);

    return arr + i * size;
}

/**
 * quick_sort_optimize
 * 功能：对任意类型的数组进行快速排序，中等规模数据表现好，有递归空间消耗
 * 参数：
 *		base	- 待排序数组的首地址
 *		nmemb	- 数组中元素的个数
 *		size	- 每个元素的大小（字节数）
 *		compar	- 比较函数的指针，用于比较两个元素
 * 返回值：无
 * 注意：选择结果是升序，比较函数需要遵循qsort的约定，不稳定排序
 * 原理：分治法，选择基准元素，将数组分为两部分，递归排序，带有迭代，减少空间复?
    ??度。
 */
void quick_sort_optimize(void *base, size_t num, size_t size, int (*compar)(const void *, const void *))
{
    if (base == NULL || num < 2 || size == 0 || compar == NULL)
    {
        return;
    }

    char *arr = (char *)base;

    while (num > 1)
    {
        void *pivot_pos = partition(arr, num, size, compar);

        size_t left_num  = ((char *)pivot_pos - arr) / size;
        size_t right_num = num - left_num - 1;

        // 递归处理较小的部分，迭代处理较大的部分
        if (left_num < right_num)
        {
            quick_sort_optimize(arr, left_num, size, compar);
            arr = (char *)pivot_pos + size;
            num = right_num;
        }
        else
        {
            quick_sort_optimize((char *)pivot_pos + size, right_num, size, compar);
            num = left_num;
        }
    }
}

/**
 * merge_sort_iterative
 * 功能：对任意类型的数组进行归并排序，适合外部排序和链表排序
 * 参数：
 *		base	- 待排序数组的首地址
 *		nmemb	- 数组中元素的个数
 *		size	- 每个元素的大小（字节数）
 *		compar	- 比较函数的指针，用于比较两个元素
 * 返回值：无
 * 注意：选择结果是升序，比较函数需要遵循qsort的约定，稳定排序
 * 原理：分治法，将数组不断二分，合并有序子数组
 */
void merge_sort_iterative(void *base, size_t num, size_t size, int (*compar)(const void *, const void *))
{
    if (base == NULL || num < 2 || size == 0 || compar == NULL)
    {
        return;
    }

    char *arr = (char *)base;

    // 申请临时空间
    char *temp_arr = (char *)malloc(num * size);

    if (temp_arr == NULL)
    {
        return;
    }

    // 宽度：当前合并的子数组的大小(1、2、4、...)
    for (size_t width = 1; width < num; width *= 2)
    {
        // 合并两个相邻宽度为width的子数组
        for (size_t i = 0; i < num; i += width * 2)
        {
            size_t left_start  = i;
            size_t left_num    = width;
            size_t right_start = i + width;
            size_t right_num   = (right_start + width > num) ? (num - right_start) : width;

            if (right_num == 0)
            {
                continue;
            }

            // 合并两个子数组的数据到临时空间
            char  *temp_ptr = temp_arr;
            size_t l        = left_start;
            size_t r        = right_start;

            while (l < left_start + left_num && r < right_start + right_num)
            {
                if (compar(arr + l * size, arr + r * size) <= 0)
                {
                    memcpy(temp_ptr, arr + l * size, size);
                    l++;
                }
                else
                {
                    memcpy(temp_ptr, arr + r * size, size);
                    r++;
                }

                temp_ptr += size;
            }

            while (l < left_start + left_num)
            {
                memcpy(temp_ptr, arr + l * size, size);
                l++;
                temp_ptr += size;
            }

            while (r < right_start + right_num)
            {
                memcpy(temp_ptr, arr + r * size, size);
                r++;
                temp_ptr += size;
            }

            // 把临时空间中合并后的数据复制回原数组
            memcpy(arr + left_start * size, temp_arr, (left_num + right_num) * size);
        }
    }

    free(temp_arr);
}
