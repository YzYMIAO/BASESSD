
#include "common.h"

typedef struct LIST_NODE_S
{
    int                 val;
    struct LIST_NODE_S *next;
} list_node_s;

static inline void free_link_list(list_node_s *list)
{
    while (list != NULL)
    {
        list_node_s *next = list->next;
        free(list);
        list = next;
    }
}

BOOL hasCycle(list_node_s *head)
{
    if (head == NULL || head->next == NULL)
    {
        return false;
    }

    list_node_s *slow = head;
    list_node_s *fast = head->next;

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

list_node_s *addTwoNumbers(list_node_s *l1, list_node_s *l2)
{
    list_node_s  dummy;
    list_node_s *tail  = &dummy;
    int          carry = 0;

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

        list_node_s *new = (list_node_s *)malloc(sizeof(list_node_s));

        if (new == NULL)
        {
            while (dummy.next != NULL)
            {
                list_node_s *next = dummy.next->next;
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

list_node_s *mergeTwoLists(list_node_s *list1, list_node_s *list2)
{
    list_node_s  dummy;
    list_node_s *tail = &dummy;

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

        list_node_s *new = (list_node_s *)malloc(sizeof(list_node_s));

        if (new == NULL)
        {
            while (dummy.next != NULL)
            {
                list_node_s *next = dummy.next->next;
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
}

/* 带 random 指针的链表节点。 */
typedef struct RANDOM_LIST_NODE_S
{
    int                        val;
    struct RANDOM_LIST_NODE_S *next;
    struct RANDOM_LIST_NODE_S *random;
} random_list_node_s;

random_list_node_s *copyRandomList(random_list_node_s *head)
{
    if (head == NULL)
    {
        return NULL;
    }

    for (random_list_node_s *node = head; node != NULL; node = node->next->next)
    {
        random_list_node_s *new_node = (random_list_node_s *)malloc(sizeof(random_list_node_s));

        if (new_node == NULL)
        {
            for (random_list_node_s *cur = head; cur != NULL; cur = cur->next)
            {
                random_list_node_s *copy = cur->next;

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

    for (random_list_node_s *node = head; node != NULL; node = node->next->next)
    {
        random_list_node_s *copy = node->next;
        copy->random             = (node->random != NULL) ? node->random->next : NULL;
    }

    random_list_node_s *copyhead = head->next;

    for (random_list_node_s *node = head; node != NULL; node = node->next)
    {
        random_list_node_s *copy = node->next;
        node->next               = node->next->next;
        copy->next               = (copy->next != NULL) ? copy->next->next : NULL;
    }

    return copyhead;
}

list_node_s *reverseBetween(list_node_s *head, int left, int right)
{
    if (head == NULL || left == right)
    {
        return NULL;
    }

    list_node_s dummy;
    dummy.next        = head;
    list_node_s *prev = &dummy;

    for (int i = 0; i < left; i++)
    {
        prev = prev->next;
    }

    list_node_s *left_node = prev->next;
    list_node_s *cur       = left_node->next;

    for (int i = left + 1; i <= right; i++)
    {
        list_node_s *next = cur->next;
        cur->next         = prev->next;
        prev->next        = cur;
        cur               = next;
    }

    left_node->next = cur;

    return dummy.next;
}

list_node_s *removeNthFromEnd(list_node_s *head, int n)
{
    // 1. 使用哑节点简化边界处理
    list_node_s dummy;
    dummy.next = head;

    // 2. 快指针先走 n 步
    list_node_s *fast = head;

    for (int i = 0; i < n; i++)
    {
        fast = fast->next;
    }

    // 3. 慢指针从哑节点开始，快慢一起走
    list_node_s *slow = &dummy;

    while (fast != NULL)
    {
        slow = slow->next;
        fast = fast->next;
    }

    // 4. 此时 slow 指向待删除节点的前一个节点
    list_node_s *toDelete = slow->next;
    slow->next            = toDelete->next;
    free(toDelete);

    return dummy.next;
}

list_node_s *deleteDuplicates(list_node_s *head)
{
    if (head == NULL)
    {
        return NULL;
    }

    list_node_s dummy;
    dummy.next        = head;
    dummy.val         = INT_MAX;
    list_node_s *prev = &dummy;

    while (prev->next != NULL && prev->next->next != NULL)
    {
        if (prev->next->val == prev->next->next->val)
        {
            int plicate_val = prev->next->val;

            while (prev->next != NULL && prev->next->val == plicate_val)
            {
                list_node_s *next = prev->next->next;
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

list_node_s *rotateRight(list_node_s *head, int k)
{
    if (head == NULL || k == 0)
    {
        return head;
    }

    int          len = 0;
    list_node_s *cur = head;
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

    list_node_s dummy;
    dummy.next = head;
    dummy.val  = INT_MAX;

    list_node_s *fast = &dummy;
    list_node_s *slow = &dummy;
    for (int i = 0; i < k; i++)
    {
        fast = fast->next;
    }

    while (fast->next != NULL)
    {
        fast = fast->next;
        slow = slow->next;
    }

    list_node_s *new_head = slow->next;
    list_node_s *new_tail = slow;
    list_node_s *old_tail = fast;

    dummy.next     = new_head;
    new_tail->next = NULL;
    old_tail->next = head;

    return dummy.next;
}

list_node_s *partition(list_node_s *head, int x)
{
    list_node_s smallhead;
    list_node_s largehead;
    smallhead.next = NULL;
    largehead.next = NULL;

    list_node_s *cur        = head;
    list_node_s *small_tail = &smallhead; // ✅ 初始化为头节点
    list_node_s *large_tail = &largehead; // ✅ 初始化为头节点

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
