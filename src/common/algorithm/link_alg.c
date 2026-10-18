
#include "common.h"

struct ListNode
{
    int              val;
    struct ListNode *next;
};

static inline void free_link_list(struct ListNode *list)
{
    while (list != NULL)
    {
        struct ListNode *next = list->next;
        free(list);
        list = next;
    }
}

bool hasCycle(struct ListNode *head)
{
    if (head == NULL || head->next == NULL)
    {
        return false;
    }

    struct ListNode *slow = head;
    struct ListNode *fast = head->next;

    while (fast != slow)
    {
        if (fast == NULL || fast->next == NULL)
        {
            return false;
        }

        slow = slow->next;
        fast = fast->next->next;
    }

    return true;
}

struct ListNode *addTwoNumbers(struct ListNode *l1, struct ListNode *l2)
{
    struct ListNode  dummy;
    struct ListNode *tail  = &dummy;
    int              carry = 0;

    while (l1 != NULL || l2 != NULL || carry)
    {
        int sum = carry;

        if (l1 != NULL)
        {
            sum += l1->val;
            l1 = l1->next;
        }

        if (l2 != NULL)
        {
            sum += l2->val;
            l2 = l2->next;
        }

        struct ListNode *new = (struct ListNode *)malloc(sizeof(struct ListNode));

        if (new == NULL)
        {
            while (dummy.next != NULL)
            {
                struct ListNode *next = dummy.next->next;
                free(dummy.next);
                dummy.next = next;
            }

            return NULL;
        }

        new->val   = sum % 10;
        new->next  = NULL;
        tail->next = new;
        tail       = new;
        carry      = sum / 10;
    }

    return dummy.next;
}

struct ListNode *mergeTwoLists(struct ListNode *list1, struct ListNode *list2)
{
    struct ListNode  dummy;
    struct ListNode *tail = &dummy;

    while (list1 != NULL || list2 != NULL)
    {
        int value = 0;

        if (list1 != NULL && list2 != NULL)
        {
            if (list1->val < list2->val)
            {
                value = list1->val;
                list1 = list1->next;
            }
            else
            {
                value = list2->val;
                list2 = list2->next;
            }
        }
        else
        {
            if (list1 != NULL)
            {
                value = list1->val;
                list1 = list1->next;
            }
            else
            {
                value = list2->val;
                list2 = list2->next;
            }
        }

        struct ListNode *new = (struct ListNode *)malloc(sizeof(struct ListNode));

        if (new == NULL)
        {
            while (dummy.next != NULL)
            {
                struct ListNode *next = dummy.next->next;
                free(dummy.next);
                dummy.next = next;
            }

            return NULL;
        }

        new->next  = NULL;
        new->val   = value;
        tail->next = new;
        tail       = tail->next;
    }

    return dummy.next;

} /**
   * Definition for a Node.
   * struct Node {
   *	   int val;
   *	   struct Node *next;
   *	   struct Node *random;
   * };
   */
struct Node
{
    int          val;
    struct Node *next;
    struct Node *random;
};

struct Node *copyRandomList(struct Node *head)
{
    if (head == NULL)
    {
        return NULL;
    }

    for (struct Node *node = head; node != NULL; node = node->next->next)
    {
        struct Node *new_node = (struct Node *)malloc(sizeof(struct Node));

        if (new_node == NULL)
        {
            for (struct Node *cur = head; cur != NULL; cur = cur->next)
            {
                struct Node *copy = cur->next;

                if (cur->next != NULL)
                {
                    cur->next = cur->next->next;
                    free(copy);
                }
            }

            return NULL;
        }

        new_node->val  = node->val;
        new_node->next = node->next;
        node->next     = new_node;
    }

    for (struct Node *node = head; node != NULL; node = node->next->next)
    {
        struct Node *copy = node->next;
        copy->random      = (node->random != NULL) ? node->random->next : NULL;
    }

    struct Node *copyhead = head->next;

    for (struct Node *node = head; node != NULL; node = node->next)
    {
        struct Node *copy = node->next;
        node->next        = node->next->next;
        copy->next        = (copy->next != NULL) ? copy->next->next : NULL;
    }

    return copyhead;
}

struct ListNode *reverseBetween(struct ListNode *head, int left, int right)
{
    if (head == NULL || left == right)
    {
        return NULL;
    }

    struct ListNode dummy;
    dummy.next            = head;
    struct ListNode *prev = &dummy;

    for (int i = 0; i < left; i++)
    {
        prev = prev->next;
    }

    struct ListNode *left_node = prev->next;
    struct ListNode *cur       = left_node->next;

    for (int i = left + 1; i <= right; i++)
    {
        struct ListNode *next = cur->next;
        cur->next             = prev->next;
        prev->next            = cur;
        cur                   = next;
    }

    left_node->next = cur;

    return dummy.next;
}

struct ListNode *removeNthFromEnd(struct ListNode *head, int n)
{
    // 1. 使用哑节点简化边界处理
    struct ListNode dummy;
    dummy.next = head;

    // 2. 快指针先走 n 步
    struct ListNode *fast = head;

    for (int i = 0; i < n; i++)
    {
        fast = fast->next;
    }

    // 3. 慢指针从哑节点开始，快慢一起走
    struct ListNode *slow = &dummy;

    while (fast != NULL)
    {
        slow = slow->next;
        fast = fast->next;
    }

    // 4. 此时 slow 指向待删除节点的前一个节点
    struct ListNode *toDelete = slow->next;
    slow->next                = toDelete->next;
    free(toDelete);

    return dummy.next;
}

struct ListNode *deleteDuplicates(struct ListNode *head)
{
    if (head == NULL)
    {
        return NULL;
    }

    struct ListNode dummy;
    dummy.next            = head;
    dummy.val             = INT_MAX;
    struct ListNode *prev = &dummy;

    while (prev->next != NULL && prev->next->next != NULL)
    {
        if (prev->next->val == prev->next->next->val)
        {
            int plicate_val = prev->next->val;

            while (prev->next != NULL && prev->next->val == plicate_val)
            {
                struct ListNode *next = prev->next->next;
                free(prev->next);
                prev->next = next;
            }
        }
        else
        {
            prev = prev->next;
        }
    }
    return dummy.next;
}

struct ListNode *rotateRight(struct ListNode *head, int k)
{
    if (head == NULL || k == 0)
    {
        return head;
    }

    int              len = 0;
    struct ListNode *cur = head;
    while (cur != NULL)
    {
        len++;
        cur = cur->next;
    }
    k = k % len;
    if (k == 0)
    {
        return head;
    }

    struct ListNode dummy;
    dummy.next = head;
    dummy.val  = INT_MAX;

    struct ListNode *fast = &dummy;
    struct ListNode *slow = &dummy;
    for (int i = 0; i < k; i++)
    {
        fast = fast->next;
    }

    while (fast->next != NULL)
    {
        fast = fast->next;
        slow = slow->next;
    }

    struct ListNode *new_head = slow->next;
    struct ListNode *new_tail = slow;
    struct ListNode *old_tail = fast;

    dummy.next     = new_head;
    new_tail->next = NULL;
    old_tail->next = head;

    return dummy.next;
}

struct ListNode *partition(struct ListNode *head, int x)
{
    struct ListNode smallhead;
    struct ListNode largehead;
    smallhead.next = NULL;
    largehead.next = NULL;

    struct ListNode *cur        = head;
    struct ListNode *small_tail = &smallhead; // ✅ 初始化为头节点
    struct ListNode *large_tail = &largehead; // ✅ 初始化为头节点

    while (cur != NULL)
    {
        if (cur->val < x)
        {
            small_tail->next = cur;
            small_tail       = small_tail->next;
        }
        else
        {
            large_tail->next = cur;
            large_tail       = large_tail->next;
        }
        cur = cur->next;
    }

    // 连接 small 和 large
    small_tail->next = largehead.next;
    large_tail->next = NULL; // ✅ 确保 large 链表尾节点断开

    return smallhead.next;
}
