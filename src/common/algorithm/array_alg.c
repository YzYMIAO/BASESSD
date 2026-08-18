#include <common.h>

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
