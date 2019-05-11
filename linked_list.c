#include <stdlib.h>
#include "linked_list.h"

linked_list_t* ll_create(void *data) {
  linked_list_t *node = (linked_list_t*)malloc(sizeof(linked_list_t));
  node->user_data = data;
  node->next = NULL;
  return node;
}

linked_list_t* ll_append(linked_list_t *head, void *user_data) {
  if (!head) {
    head = ll_create(user_data);
    return head;
  }
  linked_list_t *p = head;
  while (p->next != NULL) {
    p = p->next;
  }
  p->next = ll_create(user_data);
  return head;
}

linked_list_t *ll_poph(linked_list_t **head) {
  if (!*head) {
    return NULL;
  }
  linked_list_t *out = *head;
  if ((*head)->next) {
    *head = (*head)->next;
  } else {
    *head = NULL;
  }
  return out;
}

unsigned int ll_len(linked_list_t *head) {
  if (!head) {
    return 0;
  }
  int i = 0;
  linked_list_t *p = head;
  while (p) {
    i++;
    p = p->next;
  }
  return i;
}

void ll_free(linked_list_t *ll, free_user_data_func_t freefn) {
  linked_list_t *head;
  while (head = ll_poph(&ll)) {
    void *res = head->user_data;
    if (freefn) {
      freefn(res);
    }
    free(head);
  }
}
