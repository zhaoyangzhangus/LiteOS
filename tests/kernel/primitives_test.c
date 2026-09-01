#include <stdio.h>

#include <kernel/bitmap.h>
#include <kernel/list.h>
#include <kernel/rbtree.h>
#include <kernel/spinlock.h>

#define TEST_CHECK(condition, message) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "primitives: FAIL: %s\n", (message)); \
            return 1; \
        } \
    } while (0)

typedef struct list_test_node {
    int key;
    list_head_t link;
} list_test_node_t;

static int test_list(void) {
    list_head_t head;
    list_test_node_t first = {1, {0, 0}};
    list_test_node_t second = {2, {0, 0}};
    list_test_node_t third = {3, {0, 0}};
    list_test_node_t fourth = {4, {0, 0}};
    list_test_node_t replacement = {40, {0, 0}};
    list_head_t *cursor;
    int expected[] = {1, 2, 3, 40};
    size_t index = 0U;

    list_init(&head);
    list_init(&first.link);
    list_init(&second.link);
    list_init(&third.link);
    list_init(&fourth.link);
    list_init(&replacement.link);

    list_add(&head, &second.link);
    list_add_tail(&head, &fourth.link);
    list_add_before(&fourth.link, &third.link);
    list_add(&head, &first.link);
    list_replace(&fourth.link, &replacement.link);

    TEST_CHECK(!list_empty(&head), "list unexpectedly empty");
    list_for_each(cursor, &head) {
        list_test_node_t *node = list_entry(cursor, list_test_node_t, link);
        TEST_CHECK(index < sizeof(expected) / sizeof(expected[0]),
                   "list iteration has too many nodes");
        TEST_CHECK(node->key == expected[index], "list order mismatch");
        ++index;
    }
    TEST_CHECK(index == sizeof(expected) / sizeof(expected[0]),
               "list iteration has too few nodes");
    TEST_CHECK(list_empty(&fourth.link), "replaced list node not detached");

    list_del(&second.link);
    TEST_CHECK(list_entry(head.next, list_test_node_t, link)->key == 1,
               "list_del removed the wrong node");

    list_head_t *next;
    list_for_each_safe(cursor, next, &head) list_del(cursor);
    TEST_CHECK(list_empty(&head), "safe list deletion did not empty list");
    return 0;
}

static int test_bitmap(void) {
    uint64_t words[2] = {~0ULL, ~0ULL};
    const size_t no_bit = (size_t)-1;

    TEST_CHECK(bitmap_word_count(0U) == 0U, "zero-sized bitmap has words");
    TEST_CHECK(bitmap_word_count(70U) == 2U, "bitmap word count mismatch");
    bitmap_zero(words, 70U);
    TEST_CHECK(bitmap_find_first_set(words, 70U) == no_bit,
               "zeroed bitmap contains a set bit");
    TEST_CHECK(bitmap_find_first_zero(words, 70U) == 0U,
               "zeroed bitmap first zero mismatch");

    bitmap_set_bit(words, 0U);
    bitmap_set_bit(words, 63U);
    bitmap_set_bit(words, 69U);
    TEST_CHECK(bitmap_test_bit(words, 0U) && bitmap_test_bit(words, 63U) &&
                   bitmap_test_bit(words, 69U),
               "bitmap set bit failed");
    TEST_CHECK(bitmap_find_first_set(words, 70U) == 0U,
               "bitmap first set mismatch");
    bitmap_clear_bit(words, 0U);
    TEST_CHECK(bitmap_find_first_set(words, 70U) == 63U,
               "bitmap clear bit failed");

    bitmap_set_range(words, 0U, 70U, true);
    TEST_CHECK(bitmap_find_first_zero(words, 70U) == no_bit,
               "bitmap range set left a visible zero");
    words[1] |= ~((1ULL << 6U) - 1ULL);
    TEST_CHECK(bitmap_find_first_zero(words, 70U) == no_bit,
               "bitmap search included bits outside its size");
    bitmap_clear_bit(words, 69U);
    TEST_CHECK(bitmap_find_first_zero(words, 70U) == 69U,
               "bitmap last-bit clear mismatch");
    TEST_CHECK(bit_test_u64(1ULL << 63U, 63U) &&
                   bit_mask_u64(64U) == 0U &&
                   bit_scan_forward_u64(0x8000000000000008ULL) == 3U &&
                   bit_scan_reverse_u64(0x8000000000000008ULL) == 63U &&
                   bit_count_u64(0xF0F0ULL) == 8U,
               "bit operations mismatch");
    return 0;
}

typedef struct rb_test_node {
    uint32_t key;
    rb_node_t tree;
} rb_test_node_t;

static rb_test_node_t *rb_test_node_from_tree(rb_node_t *node) {
    return list_entry(node, rb_test_node_t, tree);
}

static bool rb_validate_node(rb_node_t *node, rb_node_t *parent,
                             bool has_min, uint32_t min_key,
                             bool has_max, uint32_t max_key,
                             unsigned *black_height, size_t *count) {
    if (node == 0) {
        *black_height = 1U;
        return true;
    }
    if (rb_tree_parent(node) != parent) return false;
    rb_test_node_t *test_node = rb_test_node_from_tree(node);
    if ((has_min && test_node->key <= min_key) ||
        (has_max && test_node->key >= max_key)) return false;
    if (rb_tree_color(node) != RB_TREE_RED &&
        rb_tree_color(node) != RB_TREE_BLACK) return false;
    if (rb_tree_color(node) == RB_TREE_RED &&
        (rb_tree_color(node->left) == RB_TREE_RED ||
         rb_tree_color(node->right) == RB_TREE_RED)) return false;

    unsigned left_height;
    unsigned right_height;
    if (!rb_validate_node(node->left, node, has_min, min_key, true,
                          test_node->key, &left_height, count) ||
        !rb_validate_node(node->right, node, true, test_node->key, has_max,
                          max_key, &right_height, count)) return false;
    if (left_height != right_height) return false;
    ++*count;
    *black_height = left_height +
                     (rb_tree_color(node) == RB_TREE_BLACK ? 1U : 0U);
    return true;
}

static bool rb_validate(rb_root_t *root, size_t expected_count) {
    size_t count = 0U;
    unsigned black_height = 0U;
    if (root->root != 0 && rb_tree_color(root->root) != RB_TREE_BLACK) {
        return false;
    }
    if (!rb_validate_node(root->root, 0, false, 0U, false, 0U,
                          &black_height, &count)) return false;
    if (count != expected_count) return false;

    size_t iterated = 0U;
    uint32_t previous = 0U;
    bool have_previous = false;
    for (rb_node_t *node = rb_tree_first(root); node != 0;
         node = rb_tree_next(node)) {
        uint32_t key = rb_test_node_from_tree(node)->key;
        if (have_previous && key <= previous) return false;
        previous = key;
        have_previous = true;
        ++iterated;
    }
    return iterated == expected_count;
}

static int test_rbtree(void) {
    static const uint32_t insertion_order[] = {
        16U, 8U, 24U, 4U, 12U, 20U, 28U, 2U, 6U, 10U, 14U,
        18U, 22U, 26U, 30U, 1U, 3U, 5U, 7U, 9U, 11U, 13U,
        15U, 17U, 19U, 21U, 23U, 25U, 27U, 29U, 0U,
    };
    static const uint32_t erase_order[] = {
        16U, 0U, 30U, 7U, 22U, 3U, 28U, 12U, 1U, 19U, 8U,
        25U, 14U, 5U, 27U, 10U, 2U, 21U, 6U, 18U, 4U, 24U,
        9U, 13U, 29U, 11U, 15U, 20U, 17U, 23U, 26U,
    };
    rb_root_t root;
    rb_test_node_t nodes[31];

    rb_tree_root_init(&root);
    for (size_t index = 0U; index < 31U; ++index) {
        nodes[index].key = (uint32_t)index;
        rb_tree_node_init(&nodes[index].tree);
    }

    for (size_t index = 0U; index < 31U; ++index) {
        rb_test_node_t *inserted = &nodes[insertion_order[index]];
        rb_node_t *parent = 0;
        rb_node_t **link = &root.root;
        while (*link != 0) {
            rb_test_node_t *current = rb_test_node_from_tree(*link);
            parent = *link;
            link = inserted->key < current->key ? &(*link)->left :
                                                   &(*link)->right;
        }
        *link = &inserted->tree;
        rb_tree_set_parent(&inserted->tree, parent);
        rb_tree_set_color(&inserted->tree, RB_TREE_RED);
        rb_tree_insert_rebalance(&root, &inserted->tree);
        TEST_CHECK(rb_validate(&root, index + 1U),
                   "red-black tree invalid after insertion");
    }

    for (size_t index = 0U; index < 31U; ++index) {
        rb_test_node_t *removed = &nodes[erase_order[index]];
        rb_tree_erase(&root, &removed->tree);
        TEST_CHECK(rb_validate(&root, 30U - index),
                   "red-black tree invalid after deletion");
        TEST_CHECK(removed->tree.left == 0 && removed->tree.right == 0 &&
                       rb_tree_parent(&removed->tree) == 0,
                   "erased red-black node was not detached");
    }
    TEST_CHECK(root.root == 0, "red-black tree did not become empty");
    return 0;
}

static int test_spinlock(void) {
    spinlock_t lock;
    spinlock_init(&lock);
    TEST_CHECK(spinlock_try_lock(&lock), "spinlock try-lock failed");
    TEST_CHECK(!spinlock_try_lock(&lock), "spinlock try-lock reentered");
    spinlock_unlock(&lock);
    TEST_CHECK(spinlock_try_lock(&lock), "spinlock did not unlock");
    spinlock_unlock(&lock);
    return 0;
}

int main(void) {
    if (test_list() != 0 || test_bitmap() != 0 || test_rbtree() != 0 ||
        test_spinlock() != 0) return 1;
    puts("primitives: ok");
    return 0;
}
