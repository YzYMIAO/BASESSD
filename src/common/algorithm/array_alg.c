

#include "common.h"

int cmp(const void *a, const void *b)
{
    int ia = *(const int *)a;
    int ib = *(const int *)b;

    return (ia > ib) - (ia < ib);
}

int *twoSum(int *nums, int numsSize, int target, int *returnSize)
{
    for (int i = 0; i < numsSize; i++)
    {
        for (int j = i + 1; j < numsSize; j++)
        {
            if ((nums[i] + nums[j]) == target)
            {
                *returnSize     = 2;
                int *target_idx = malloc(*returnSize * sizeof(int));

                if (target_idx != NULL)
                {
                    target_idx[0] = i;
                    target_idx[1] = j;
                    return target_idx;
                }
            }
        }
    }

    *returnSize = 0;
    return NULL;
}

double findMedianSortedArrays(int *nums1, int nums1Size, int *nums2, int nums2Size)
{
    int total = nums1Size + nums2Size;
    int mid   = total / 2;  // 中位索引（偶数时是右中位）
    int prev = 0, curr = 0; // 记录归并的值
    int p1 = 0, p2 = 0;

    // 归并到中位位置（包含中位）
    for (int i = 0; i <= mid; i++)
    {
        prev = curr; // 保存上一个值（用于偶数情况）

        if (p1 >= nums1Size)
        {
            curr = nums2[p2++];
        }
        else if (p2 >= nums2Size)
        {
            curr = nums1[p1++];
        }
        else if (nums1[p1] <= nums2[p2])
        {
            curr = nums1[p1++];
        }
        else
        {
            curr = nums2[p2++];
        }
    }

    // 奇数：直接返回中位
    if (total & 0x1)
    {
        return curr;
    }

    // 偶数：返回两个中位的平均
    else
    {
        return (prev + curr) / 2.0;
    }
}

int maxArea(int *height, int heightSize)
{
    int curr_area = 0;
    int max_area  = 0;

    for (int l = 0, r = heightSize - 1; l < r;)
    {
        int left_h  = height[l];
        int right_h = height[r];

        int len  = r - l;
        int wide = 0;

        if (left_h < right_h)
        {
            l++;
            wide = left_h;
        }
        else
        {
            r--;
            wide = right_h;
        }

        curr_area = wide * len;

        max_area = (max_area > curr_area) ? max_area : curr_area;
    }

    return max_area;
}

int **threeSum(int *nums, int numsSize, int *returnSize, int **returnColumnSizes)
{
    // 1. 参数检查
    if (nums == NULL || numsSize < 3)
    {
        *returnSize = 0;
        return NULL;
    }

    // 2. 冒泡排序
    for (int i = 0; i < numsSize - 1; i++)
    {
        BOOL swapped = false;

        for (int j = 0; j < numsSize - 1 - i; j++)
        {
            if (nums[j] > nums[j + 1])
            {
                int tmp = nums[j];

                nums[j]     = nums[j + 1];
                nums[j + 1] = tmp;
                swapped     = true;
            }
        }

        if (!swapped)
        {
            break;
        }
    }

    // 3. 初始容量（经验值：大多数情况够用）
    int   capacity = 16; // 初始容量 16 个三元组
    int **result   = (int **)malloc(sizeof(int *) * capacity);

    if (result == NULL)
    {
        *returnSize = 0;
        return NULL;
    }

    *returnColumnSizes = (int *)malloc(sizeof(int) * capacity);

    if (*returnColumnSizes == NULL)
    {
        free(result);
        *returnSize = 0;
        return NULL;
    }

    *returnSize = 0;

    // 4. 三数之和（一次扫描）
    for (int i = 0; i < numsSize - 2; i++)
    {
        // 剪枝
        if (nums[i] > 0)
        {
            break;
        }

        // 去重
        if (i > 0 && nums[i] == nums[i - 1])
        {
            continue;
        }

        int left  = i + 1;
        int right = numsSize - 1;

        while (left < right)
        {
            int sum = nums[i] + nums[left] + nums[right];

            if (sum < 0)
            {
                left++;
            }
            else if (sum > 0)
            {
                right--;
            }
            else
            {
                // ============================================
                // 关键：检查容量，不够就扩容（2倍增长）
                // ============================================
                if (*returnSize >= capacity)
                {
                    int new_capacity = capacity * 2; // 2倍扩容

                    // 扩容 result
                    int **new_result = (int **)realloc(result, sizeof(int *) * new_capacity);

                    if (new_result == NULL)
                    {
                        // 扩容失败，释放已分配的内存
                        for (int k = 0; k < *returnSize; k++)
                        {
                            free(result[k]);
                        }

                        free(result);
                        free(*returnColumnSizes);
                        *returnSize = 0;
                        return NULL;
                    }

                    result = new_result;

                    // 扩容 returnColumnSizes
                    int *new_col_sizes = (int *)realloc(*returnColumnSizes, sizeof(int) * new_capacity);

                    if (new_col_sizes == NULL)
                    {
                        // 扩容失败，释放已分配的内存
                        for (int k = 0; k < *returnSize; k++)
                        {
                            free(result[k]);
                        }

                        free(result);
                        free(*returnColumnSizes);
                        *returnSize = 0;
                        return NULL;
                    }

                    *returnColumnSizes = new_col_sizes;

                    capacity = new_capacity;
                }

                // ============================================
                // 存储解
                // ============================================
                result[*returnSize] = (int *)malloc(sizeof(int) * 3);

                if (result[*returnSize] == NULL)
                {
                    // 错误处理
                    for (int k = 0; k < *returnSize; k++)
                    {
                        free(result[k]);
                    }

                    free(result);
                    free(*returnColumnSizes);
                    *returnSize = 0;
                    return NULL;
                }

                result[*returnSize][0]            = nums[i];
                result[*returnSize][1]            = nums[left];
                result[*returnSize][2]            = nums[right];
                (*returnColumnSizes)[*returnSize] = 3;
                (*returnSize)++;

                // 去重
                int leftVal  = nums[left];
                int rightVal = nums[right];

                while (left < right && nums[left] == leftVal)
                {
                    left++;
                }

                while (left < right && nums[right] == rightVal)
                {
                    right--;
                }
            }
        }
    }

    return result;
}

int threeSumClosest(int *nums, int numsSize, int target)
{
    qsort(nums, numsSize, sizeof(int), cmp);

    int closest  = nums[0] + nums[1] + nums[2];
    int min_diff = abs(closest - target);

    // ✅ 如果已经完全匹配，直接返回
    if (min_diff == 0)
    {
        return closest;
    }

    for (int i = 0; i < numsSize - 2; i++)
    {
        if (i > 0 && nums[i] == nums[i - 1])
        {
            continue;
        }

        int left  = i + 1;
        int right = numsSize - 1;

        while (left < right)
        {
            long long sum_ll = (long long)nums[i] + nums[left] + nums[right];
            int       sum    = (int)sum_ll;

            int diff = sum > target ? sum - target : target - sum;

            if (diff < min_diff)
            {
                min_diff = diff;
                closest  = sum;

                if (min_diff == 0)
                {
                    return closest; // ✅ 提前返回
                }
            }

            if (sum < target)
            {
                left++;

                while (left < right && nums[left] == nums[left - 1])
                {
                    left++;
                }
            }
            else if (sum > target)
            {
                right--;

                while (left < right && nums[right] == nums[right + 1])
                {
                    right--;
                }
            }
            else
            {
                return sum;
            }
        }
    }

    return closest;
}

int **fourSum(int *nums, int numsSize, int target, int *returnSize, int **returnColumnSizes)
{
    if (nums == NULL || numsSize < 4)
    {
        *returnSize = 0;
        return NULL;
    }

    int   capacity = 16;
    int **result   = (int **)malloc(sizeof(int *) * capacity);

    if (result == NULL)
    {
        *returnSize = 0;
        return NULL;
    }

    *returnColumnSizes = (int *)malloc(sizeof(int) * capacity);

    if (*returnColumnSizes == NULL)
    {
        free(result);
        *returnSize = 0;
        return NULL;
    }

    *returnSize = 0;

    // 排序
    qsort(nums, numsSize, sizeof(int), cmp);

    for (int i = 0; i < numsSize - 3; i++)
    {
        // 去重
        if (i > 0 && nums[i] == nums[i - 1])
        {
            continue;
        }

        // 剪枝：最小可能和
        long long min_sum_i = (long long)nums[i] + nums[i + 1] + nums[i + 2] + nums[i + 3];

        if (min_sum_i > target)
        {
            break;
        }

        // 剪枝：最大可能和
        long long max_sum_i = (long long)nums[i] + nums[numsSize - 1] + nums[numsSize - 2] + nums[numsSize - 3];

        if (max_sum_i < target)
        {
            continue;
        }

        for (int j = i + 1; j < numsSize - 2; j++)
        {
            // 去重
            if (j > i + 1 && nums[j] == nums[j - 1])
            {
                continue;
            }

            // 剪枝：最小可能和
            long long min_sum_j = (long long)nums[i] + nums[j] + nums[j + 1] + nums[j + 2];

            if (min_sum_j > target)
            {
                break;
            }

            // 剪枝：最大可能和
            long long max_sum_j = (long long)nums[i] + nums[j] + nums[numsSize - 1] + nums[numsSize - 2];

            if (max_sum_j < target)
            {
                continue;
            }

            int left  = j + 1;
            int right = numsSize - 1;

            while (left < right)
            {
                long long sum = (long long)nums[i] + nums[j] + nums[left] + nums[right];

                if (sum < target)
                {
                    left++;
                }
                else if (sum > target)
                {
                    right--;
                }
                else
                {
                    // ✅ 检查容量
                    if (*returnSize >= capacity)
                    {
                        int new_capacity = capacity * 2;

                        int **new_result = (int **)realloc(result, sizeof(int *) * new_capacity);

                        if (new_result == NULL)
                        {
                            // 错误处理
                            for (int k = 0; k < *returnSize; k++)
                            {
                                free(result[k]);
                            }

                            free(result);
                            free(*returnColumnSizes);
                            *returnSize = 0;
                            return NULL;
                        }

                        result = new_result;

                        int *new_col_sizes = (int *)realloc(*returnColumnSizes, sizeof(int) * new_capacity);

                        if (new_col_sizes == NULL)
                        {
                            for (int k = 0; k < *returnSize; k++)
                            {
                                free(result[k]);
                            }

                            free(result);
                            free(*returnColumnSizes);
                            *returnSize = 0;
                            return NULL;
                        }

                        *returnColumnSizes = new_col_sizes;
                        capacity           = new_capacity;
                    }

                    // ✅ 正确存储
                    result[*returnSize] = (int *)malloc(4 * sizeof(int));

                    if (result[*returnSize] == NULL)
                    {
                        for (int k = 0; k < *returnSize; k++)
                        {
                            free(result[k]);
                        }

                        free(result);
                        free(*returnColumnSizes);
                        *returnSize = 0;
                        return NULL;
                    }

                    result[*returnSize][0]            = nums[i];
                    result[*returnSize][1]            = nums[j];
                    result[*returnSize][2]            = nums[left];
                    result[*returnSize][3]            = nums[right];
                    (*returnColumnSizes)[*returnSize] = 4; // ✅ 修正！
                    (*returnSize)++;

                    left++;

                    while (left < right && nums[left] == nums[left - 1])
                    {
                        left++;
                    }

                    // 2. 右指针去重
                    right--;

                    while (left < right && nums[right] == nums[right + 1])
                    {
                        right--;
                    }
                }
            }
        }
    }

    return result;
}

int removeDuplicates(int *nums, int numsSize)
{
    if (nums == NULL || numsSize == 0)
    {
        return 0;
    }

    int first  = 0;
    int secend = 0;

    while (first < numsSize)
    {
        nums[secend++] = nums[first++];

        while (first < numsSize && nums[first] == nums[secend - 1])
        {
            first++;
        }
    }

    return secend;
}

int removeElement(int *nums, int numsSize, int val)
{
    if (nums == NULL || numsSize == 0)
    {
        return 0;
    }

    int first  = 0;
    int secend = 0;

    while (first < numsSize)
    {
        while (first < numsSize && nums[first] == val)
        {
            first++;
        }

        if (first < numsSize)
        {
            nums[secend++] = nums[first++];
        }
        else
        {
            break;
        }
    }

    return secend;
}

static void swap(int *a, int *b)
{
    int temp = *a;

    *a = *b;
    *b = temp;
}

void reverse(int *arr, int left_idx, int right_idx)
{
    while (left_idx < right_idx)
    {
        swap(&arr[left_idx], &arr[right_idx]); // ✅ 用 & 取地址
        left_idx++;
        right_idx--;
    }
}

void nextPermutation(int *nums, int numsSize)
{
    if (nums == NULL || numsSize < 2)
    {
        return;
    }

    // 1. 从右往左找第一个升序对 (nums[i] < nums[i+1])
    int first_low_index = -1;

    for (int i = numsSize - 2; i >= 0; i--)
    {
        if (nums[i] < nums[i + 1])
        {
            first_low_index = i;
            break;
        }
    }

    // 2. 如果找到了升序对
    if (first_low_index != -1)
    {
        // 从右往左找第一个大于 nums[first_low_index] 的元素
        for (int j = numsSize - 1; j > first_low_index; j--)
        {
            if (nums[j] > nums[first_low_index])
            {
                swap(&nums[first_low_index], &nums[j]);
                break;
            }
        }
    }

    // 3. 反转 first_low_index + 1 到末尾
    reverse(nums, first_low_index + 1, numsSize - 1);
}

void merge(int *nums1, int nums1Size, int m, int *nums2, int nums2Size, int n)
{
    (void)nums1Size;
    (void)nums2Size;
    int first  = m - 1;
    int secend = n - 1;
    int tail   = n + m - 1;

    while (first >= 0 && secend >= 0)
    {
        if (nums1[first] > nums2[secend])
        {
            nums1[tail--] = nums1[first--];
        }
        else
        {
            nums1[tail--] = nums2[secend--];
        }
    }

    if (secend >= 0)
    {
        memcpy(nums1, nums2, (secend + 1) * sizeof(int));
    }
}

int removeDuplicatesAllowTwice(int *nums, int numsSize)
{
    if (numsSize <= 2)
    {
        return numsSize;
    }

    int slow = 2;

    for (int fast = 2; fast < numsSize; fast++)
    {
        if (nums[fast] != nums[slow - 2])
        {
            nums[slow++] = nums[fast];
        }
    }

    return slow;
}

int majorityElement(int *nums, int numsSize)
{
    int candidate = nums[0];
    int count     = 1;

    for (int i = 1; i < numsSize; i++)
    {
        if (nums[i] == candidate)
        {
            count++;
        }
        else
        {
            count--;

            if (count == 0)
            {
                candidate = nums[i];
                count     = 1;
            }
        }
    }

    return candidate;
}

void arr_reverse(int *nums, int left, int right)
{
    while (left < right)
    {
        int tmp = nums[left];

        nums[left]  = nums[right];
        nums[right] = tmp;
        left++;
        right--;
    }
}

void rotate(int *nums, int numsSize, int k)
{
    if (nums == NULL || numsSize == 0 || k == 0)
    {
        return;
    }

    k = k % numsSize;

    arr_reverse(nums, 0, numsSize - 1);
    arr_reverse(nums, 0, k - 1);
    arr_reverse(nums, k, numsSize - 1);
}

int maxProfit(int *prices, int pricesSize)
{
    if (prices == NULL || pricesSize < 1)
    {
        return 0;
    }

    int min_price  = prices[0]; // 记录当前遇到的最低价格
    int max_profit = 0;

    for (int i = 1; i < pricesSize; i++)
    {
        // 更新最低价格
        if (prices[i] < min_price)
        {
            min_price = prices[i];
        }

        // 计算当前利润
        int profit = prices[i] - min_price;

        if (profit > max_profit)
        {
            max_profit = profit;
        }
    }

    return max_profit;
}

int maxProfit_all(int *prices, int pricesSize)
{
    int left       = 0;
    int right      = 0;
    int max_profit = 0;

    while (left < pricesSize)
    {
        right = left + 1;

        while (right < pricesSize && prices[right] > prices[right - 1])
        {
            right++;
        }

        int profit = prices[right - 1] - prices[left];

        max_profit += profit;
        left = right;
    }

    return max_profit;
}

BOOL canJump(int *nums, int numsSize)
{
    if (nums == NULL || numsSize == 0)
    {
        return false;
    }

    if (numsSize == 1)
    {
        return true;
    }

    int last = numsSize - 1;

    for (int i = numsSize - 2; i >= 0; i--)
    {
        if (i + nums[i] >= last)
        {
            last = i;
        }
    }

    return last == 0;
}

int jump(int *nums, int numsSize)
{
    if (nums == NULL || numsSize == 0)
    {
        return 0;
    }

    int step    = 0;
    int max_pos = 0;
    int end     = 0;

    for (int i = 0; i < numsSize - 1; i++)
    {
        max_pos = (max_pos > i + nums[i]) ? max_pos : i + nums[i];

        if (i == end)
        {
            end = max_pos;
            ++step;
        }
    }

    return step;
}

int hIndex(int *citations, int citationsSize)
{
    if (citations == NULL || citationsSize == 0)
    {
        return 0;
    }

    int left  = 0;
    int right = citationsSize;

    while (left < right)
    {
        int mid   = (left + right + 1) >> 1;
        int count = 0;

        for (int i = 0; i < citationsSize; i++)
        {
            if (citations[i] >= mid)
            {
                count++;
            }
        }

        if (count >= mid)
        {
            left = mid;
        }
        else
        {
            right = mid - 1;
        }
    }

    return left;
}

int *productExceptSelf(int *nums, int numsSize, int *returnSize)
{
    if (nums == NULL || numsSize == 0)
    {
        return NULL;
    }

    int *result = malloc(numsSize * sizeof(int));

    if (result == NULL)
    {
        return NULL;
    }

    result[numsSize - 1] = 1;

    for (int i = numsSize - 2; i >= 0; i--)
    {
        result[i] = result[i + 1] * nums[i + 1];
    }

    int pre = 1;

    for (int i = 0; i < numsSize; i++)
    {
        result[i] *= pre;
        pre *= nums[i];
    }

    *returnSize = numsSize;
    return result;
}

DWORD canCompleteCircuit(int *gas, int gasSize, int *cost, int costSize)
{
    (void)costSize;      // 避免编译器警告
    int total_gas   = 0; // 总油量差
    int current_gas = 0; // 当前油箱油量
    int start       = 0; // 可能的起点

    for (int i = 0; i < gasSize; i++)
    {
        int diff = gas[i] - cost[i];

        total_gas += diff;
        current_gas += diff;

        // 如果当前油量不够到下一个加油站
        if (current_gas < 0)
        {
            // 起点不可能在 [start, i] 之间，只能从 i+1 开始
            start       = i + 1;
            current_gas = 0; // 重置当前油量
        }
    }

    // 如果总油量 < 总消耗，不可能走完
    return (total_gas >= 0) ? (DWORD)start : UINT32_MAX; /* 显式「不可达」哨兵；start 是非负 int，需转无符号 */
}

int candy(int *ratings, int ratingsSize)
{
    int ret = 1;
    int inc = 1;
    int dec = 0;
    int pre = 1;

    for (int i = 1; i < ratingsSize; i++)
    {
        if (ratings[i] >= ratings[i - 1])
        {
            dec = 0;
            pre = (ratings[i] == ratings[i - 1]) ? 1 : pre + 1;
            ret += pre;
            inc = pre;
        }
        else
        {
            dec++;

            if (dec == inc)
            {
                dec++;
            }

            ret += dec;
            pre = 1;
        }
    }

    return ret;
}

int trap(int *height, int heightSize)
{
    int left   = 0;
    int right  = heightSize - 1;
    int lh_max = 0;
    int rh_max = 0;
    int total  = 0;

    while (left < right)
    {
        lh_max = (lh_max > height[left]) ? lh_max : height[left];
        rh_max = (rh_max > height[right]) ? rh_max : height[right];

        if (height[left] > height[right])
        {
            total += rh_max - height[right];
            right--;
        }
        else
        {
            total += lh_max - height[left];
            left++;
        }
    }

    return total;
}
