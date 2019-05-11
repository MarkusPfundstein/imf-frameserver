#ifndef LINKED_LIST_H
#define LINKED_LIST_H

typedef struct linked_list {
    void *user_data;
    struct linked_list *next;
} linked_list_t;

typedef void (*free_user_data_func_t)(void *);

extern linked_list_t* ll_create(void *data);
extern linked_list_t* ll_append(linked_list_t *head, void *user_data);
extern linked_list_t *ll_poph(linked_list_t **head);
extern void ll_free(linked_list_t *head, free_user_data_func_t freefn);
extern unsigned int ll_len(linked_list_t *head);

#endif
