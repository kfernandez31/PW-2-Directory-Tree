#include "Tree.h"
#include "HashMap.h"
#include "path_utils.h"
#include "safe_allocations.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <pthread.h>

#define READER 1
#define WRITER 0

/** Generic success code **/
#define SUCCESS 0
/** Error code for when an ancestor is being moved to its descendant **/
#define EMOVINGANCESTOR (-1)

/** Checks if the directory represents the root **/
#define IS_ROOT(path) (strcmp(path, "/") == 0)

/** Checks whether the result of a pthread_* function is 0 (SUCCESS) **/
#define PTHREAD_CHECK(x)                                                          \
    do {                                                                          \
        int err = (x);                                                            \
        if (err != SUCCESS) {                                                     \
            fprintf(stderr, "Runtime error: %s returned %d in %s at %s:%d\n%s\n", \
                #x, err, __func__, __FILE__, __LINE__, strerror(err));            \
            exit(EXIT_FAILURE);                                                   \
        }                                                                         \
    } while (0)

/** Performs a block of code under the node's mutex **/
#define UNDER_MUTEX(mutex, code_block)           \
do {                                             \
    PTHREAD_CHECK(pthread_mutex_lock(mutex));    \
    code_block;                                  \
    PTHREAD_CHECK(pthread_mutex_unlock(mutex));  \
} while(0);

struct Tree {
    Tree* parent;                            /** Parent directory. NULL for the root **/
    HashMap* subdirectories;                 /** HashMap of (name, node) pairs, where node is of type Tree **/
    pthread_mutex_t var_protection;          /** Mutual exclusion for variable access **/
    pthread_cond_t reader_cond;              /** Condition to hang readers **/
    pthread_cond_t writer_cond;              /** Condition to hang writers **/
    pthread_cond_t subtree_cond;             /** Condition to wait on until all subtree operations finish **/
    size_t r_count, w_count, r_wait, w_wait; /** Counters of active and waiting readers/writers **/
    size_t refcount;                         /** Reference count of operations currently performed in the subtree **/
};

/**
 * Removes and returns a subdirectory of the `tree` with the specified name.
 * @param tree : file tree
 * @param name : subdirectory name
 * @return : pointer to the subdirectory
 */
static inline Tree* pop_subdir(Tree* tree, const char* name) {
    Tree* subdir = hmap_get(tree->subdirectories, name);
    hmap_remove(tree->subdirectories, name);
    return subdir;
}

/**
 * Gets the number of immediate subdirectories / tree children.
 * @param tree : file tree
 * @return : number of subdirectories
 */
static inline size_t subdir_count(Tree* tree) {
    return hmap_size(tree->subdirectories);
}

/**
 * Called by a read-type operation to lock the tree for reading.
 * Waits if there are other active or waiting writers.
 * @param tree : file tree
 */
static void reader_lock(Tree* tree) {
    UNDER_MUTEX(&tree->var_protection,
        if (tree->w_wait || tree->w_count) {
            tree->r_wait++;
            do {
                PTHREAD_CHECK(pthread_cond_wait(&tree->reader_cond, &tree->var_protection));
            } while (tree->w_count > 0);
            tree->r_wait--;
        }
        assert(tree->w_count == 0);
        tree->r_count++;
    );
}

/**
 * Called by a read-type operation to unlock the tree from reading.
 * @param tree : file tree
 */
static void reader_unlock(Tree* tree) {
    UNDER_MUTEX(&tree->var_protection,
        assert(tree->r_count > 0);
        assert(tree->w_count == 0);
        tree->r_count--;

        if (tree->r_count == 0)
            PTHREAD_CHECK(pthread_cond_signal(&tree->writer_cond));
    );
}

/**
 * Called by a write-type operation to lock the tree for writing.
 * Waits if there are other active readers or writers.
 * @param tree : file tree
 */
static void writer_lock(Tree* tree) {
    UNDER_MUTEX(&tree->var_protection,
        while (tree->r_count || tree->w_count) {
            tree->w_wait++;
            PTHREAD_CHECK(pthread_cond_wait(&tree->writer_cond, &tree->var_protection));
            tree->w_wait--;
        }
        assert(tree->r_count == 0);
        assert(tree->w_count == 0);
        tree->w_count++;
    );
}

/**
 * Called by a write-type operation to unlock the tree from writing.
 * @param tree : file tree
 */
static void writer_unlock(Tree* tree) {
    UNDER_MUTEX(&tree->var_protection,
        assert(tree->w_count == 1);
        assert(tree->r_count == 0);
        tree->w_count--;

        if (tree->r_wait > 0)
            PTHREAD_CHECK(pthread_cond_broadcast(&tree->reader_cond));
        else
            PTHREAD_CHECK(pthread_cond_signal(&tree->writer_cond));
    );
}

/**
 * Waits for all operations to finish in the subtree of the `node`.
 * @param node : node in a file tree
 */
static void wait_until_subtree_activity_ceases(Tree* node) {
    UNDER_MUTEX(&node->var_protection, // This is only to satisfy `pthread_cond_wait`
        while (node->refcount > 0)     // Wait if necessary
            PTHREAD_CHECK(pthread_cond_wait(&node->subtree_cond, &node->var_protection));
    );
}

/**
 * Performs a cleanup along the path - decrements reference counters from `start` to `end` inclusive.
 * @param start : first node on the path
 * @param end : last node on the path
 */
static void unwind_path(Tree *start, Tree *end) {
    Tree* next = NULL;
    while (start != end){
        UNDER_MUTEX(&start->var_protection,
            next = start->parent;
            start->refcount--;
            if (start->refcount == 0)
                PTHREAD_CHECK(pthread_cond_signal(&start->subtree_cond));
        );
        start = next;
    }
}

/**
 * Gets a pointer to the directory in the `tree` specified by the `path`.
 * Locks the directory according to the `reader` flag.
 * Doesn't lock the node it starts the search from if `lock_start` is false.
 * @param tree : file tree
 * @param path : file path
 * @param start_locked : flag for locking the start node
 * @param reader : flag for locking the directory as a reader or as a writer
 * @return : pointer to the requested directory
 */
static Tree* get_node(Tree* tree, const char* path, bool start_locked, const bool reader) {
    char child_name[MAX_FOLDER_NAME_LENGTH + 1];
    Tree* end = NULL;

    if (!start_locked) {
        if (IS_ROOT(path) && !reader)
            writer_lock(tree);
        else
            reader_lock(tree);

        UNDER_MUTEX(&tree->var_protection, tree->refcount++);
        end = tree->parent;
    }

    while ((path = split_path(path, child_name))) {
        Tree* subtree = hmap_get(tree->subdirectories, child_name);
        if (subtree == NULL) {
            unwind_path(tree, end);
            if (!start_locked)
                reader_unlock(tree);
            return NULL;
        }
        if (IS_ROOT(path) && !reader) // Last node in the path
            writer_lock(subtree);
        else
            reader_lock(subtree);
        UNDER_MUTEX(&subtree->var_protection, subtree->refcount++);
        if (!start_locked)
            reader_unlock(tree);
        else
            start_locked = false;
        tree = subtree;
    }
    return tree;
}

Tree* tree_new() {
    Tree* tree = safe_calloc(1, sizeof(Tree));
    tree->subdirectories = hmap_new();
    PTHREAD_CHECK(pthread_mutex_init(&tree->var_protection, NULL));
    PTHREAD_CHECK(pthread_cond_init(&tree->reader_cond, NULL));
    PTHREAD_CHECK(pthread_cond_init(&tree->writer_cond, NULL));
    PTHREAD_CHECK(pthread_cond_init(&tree->subtree_cond, NULL));

    return tree;
}

void tree_free(Tree* tree) {
    const char* key = NULL;
    void* value = NULL;
    HashMapIterator it = hmap_iterator(tree->subdirectories);

    while (hmap_next(tree->subdirectories, &it, &key, &value)) {
        Tree* subdir = pop_subdir(tree, key);
        tree_free(subdir);
    }

    hmap_free(tree->subdirectories);
    PTHREAD_CHECK(pthread_cond_destroy(&tree->writer_cond));
    PTHREAD_CHECK(pthread_cond_destroy(&tree->reader_cond));
    PTHREAD_CHECK(pthread_cond_destroy(&tree->subtree_cond));
    PTHREAD_CHECK(pthread_mutex_destroy(&tree->var_protection));
    free(tree);
    tree = NULL;
}

char* tree_list(Tree* tree, const char* path) {
    if (!is_valid_path(path))
        return NULL;

    char* result = NULL;
    Tree* dir = get_node(tree, path, false, READER);
    if (!dir) {
        return NULL; // The directory doesn't exist
    }

    result = make_map_contents_string(dir->subdirectories); // The read

    unwind_path(dir, NULL);
    reader_unlock(dir);
    return result;
}

int tree_create(Tree* tree, const char* path) {
    if (!is_valid_path(path))
        return EINVAL; // Invalid path
    if (IS_ROOT(path))
        return EEXIST; // The root always exists

    char child_name[MAX_FOLDER_NAME_LENGTH + 1], parent_path[MAX_PATH_LENGTH + 1];
    make_path_to_parent(path, child_name, parent_path);

    Tree* parent = get_node(tree, parent_path, false, WRITER);
    if (!parent) {
        return ENOENT; // The directory's parent doesn't exist
    }

    Tree* child = tree_new();
    child->parent = parent;
    if (!hmap_insert(parent->subdirectories, child_name, child)) {
        unwind_path(parent, NULL);
        writer_unlock(parent);
        tree_free(child);
        return EEXIST; // The directory already exists
    }

    unwind_path(parent, NULL);
    writer_unlock(parent);
    return SUCCESS;
}

int tree_remove(Tree* tree, const char* path) {
    if (IS_ROOT(path))
        return EBUSY; // Cannot remove the root

    char child_name[MAX_FOLDER_NAME_LENGTH + 1], parent_path[MAX_PATH_LENGTH + 1];
    make_path_to_parent(path, child_name, parent_path);

    Tree* parent = get_node(tree, parent_path, false, WRITER);
    if (!parent) {
        return ENOENT; // The directory's parent doesn't exist
    }

    Tree* child = hmap_get(parent->subdirectories, child_name);
    if (!child) {
        unwind_path(parent, NULL);
        writer_unlock(parent);
        return ENOENT; // The directory doesn't exist
    }
    writer_lock(child);

    if (subdir_count(child) > 0) {
        writer_unlock(child);
        unwind_path(parent, NULL);
        writer_unlock(parent);
        return ENOTEMPTY; // The directory is not empty
    }
    pop_subdir(parent, child_name); // The removal

    writer_unlock(child);
    unwind_path(parent, NULL);
    writer_unlock(parent);
    tree_free(child);
    return SUCCESS;
}

int tree_move(Tree* tree, const char* s_path, const char* t_path) {
    if (!is_valid_path(s_path) || !is_valid_path(t_path))
        return EINVAL; // Invalid path names
    if (IS_ROOT(s_path))
        return EBUSY; // Can't move the root
    if (IS_ROOT(t_path))
        return EEXIST; // Can't assign a new root
    if (is_ancestor(s_path, t_path))
        return EMOVINGANCESTOR; // No directory can be moved to its descendant

    int cmp;
    size_t index_after_lca;
    char s_name[MAX_FOLDER_NAME_LENGTH + 1], t_name[MAX_FOLDER_NAME_LENGTH + 1];
    char s_parent_path[MAX_PATH_LENGTH + 1], t_parent_path[MAX_PATH_LENGTH + 1], lca_path[MAX_PATH_LENGTH + 1];
    Tree *s_dir = NULL, *s_parent = NULL, *t_parent = NULL, *lca = NULL;
    make_path_to_parent(s_path, s_name, s_parent_path);
    make_path_to_parent(t_path, t_name, t_parent_path);
    make_path_to_LCA(s_path, t_path, lca_path);
    // Get the LCA of both directories
    if (!(lca = get_node(tree, lca_path, false, WRITER))) {
        return ENOENT; // Non-existent paths
    }
    index_after_lca = strlen(lca_path) - 1;
    // Determine whether to lock two nodes
    cmp = strcmp(s_parent_path, t_parent_path);
    if (cmp != 0) {
        #define CLEANUP()                       \
            do {                                \
                if (s_parent != lca) {          \
                    unwind_path(s_parent, lca); \
                    writer_unlock(s_parent);    \
                }                               \
                if (t_parent != lca) {          \
                    unwind_path(t_parent, lca); \
                    writer_unlock(t_parent);    \
                }                               \
                unwind_path(lca, NULL);         \
                writer_unlock(lca);             \
            } while (0)

        if (!(s_parent = get_node(lca, s_parent_path + index_after_lca, true, WRITER))) {
            writer_unlock(lca);
            return ENOENT; // The source's parent doesn't exist
        }
        if (!(t_parent = get_node(lca, t_parent_path + index_after_lca, true, WRITER))) {
            if (s_parent != lca) {
                unwind_path(s_parent, lca);
                writer_unlock(s_parent);
            }
            writer_unlock(lca);
            return ENOENT; // The source's parent doesn't exist
        }
        // Find source
        if (!(s_dir = hmap_get(s_parent->subdirectories, s_name))) {
            CLEANUP();
            return ENOENT; // The source doesn't exist
        }
        // Check if target already exists
        if (hmap_get(t_parent->subdirectories, t_name)) {
            if (is_ancestor(s_path, t_path)) {
                CLEANUP();
                return EMOVINGANCESTOR; // No directory can be moved to its descendant
            }
            CLEANUP();
            return EEXIST; // There already exists a directory with the same name as the target
        }
        wait_until_subtree_activity_ceases(s_dir);
        // Pop and insert the source
        pop_subdir(s_parent, s_name);
        s_dir->parent = t_parent;
        hmap_insert(t_parent->subdirectories, t_name, s_dir);
        CLEANUP();
        #undef CLEANUP
    }
    else {
        #define CLEANUP()                       \
            do {                                \
                if (s_parent != lca) {          \
                    unwind_path(s_parent, lca); \
                    writer_unlock(s_parent);    \
                }                               \
                unwind_path(lca, NULL);         \
                writer_unlock(lca);             \
            } while (0)

        if (!(s_parent = get_node(lca, s_parent_path + index_after_lca, true, WRITER))) {
            unwind_path(lca, NULL);
            writer_unlock(lca);
            return ENOENT; // The source's parent doesn't exist
        }
        t_parent = s_parent;
        // Find source
        if (!(s_dir = hmap_get(s_parent->subdirectories, s_name))) {
            CLEANUP();
            return ENOENT; // The source doesn't exist
        }
        // Check if target already exists
        if (hmap_get(t_parent->subdirectories, t_name)) {
            if (strcmp(s_path, t_path) == 0) {
                CLEANUP();
                return SUCCESS; // The source and target are the same - nothing to move
            }
            if (is_ancestor(s_path, t_path)) {
                CLEANUP();
                return EMOVINGANCESTOR; // No directory can be moved to its descendant
            }
            CLEANUP();
            return EEXIST; // There already exists a directory with the same name as the target
        }
        wait_until_subtree_activity_ceases(s_dir);
        // Pop and insert the source
        s_dir = pop_subdir(s_parent, s_name);
        hmap_insert(t_parent->subdirectories, t_name, s_dir);
        s_dir->parent = t_parent;
        CLEANUP();
    }
    return SUCCESS;
}