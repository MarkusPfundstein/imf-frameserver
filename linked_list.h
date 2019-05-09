#ifndef LINKED_LIST_H
#define LINKED_LIST_H

typedef struct linked_list {
    void *user_data;
    struct linked_list *next;
} linked_list_t;

extern linked_list_t* ll_create(void *data);
extern void ll_append(linked_list_t *head, void *user_data);
extern linked_list_t *ll_poph(linked_list_t **head);
extern unsigned int ll_len(linked_list_t *head);

#endif
