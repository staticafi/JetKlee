//
// Created by capso on 05.11.21.
//

#include <assert.h>
#include <stdio.h>

typedef struct node {
  int val;
  struct node *next;
} node_t;

//node_t* node = NULL;

int get_sign(node_t* node) {
  //node_t* next = node->next;
  int next_val = node->next->val;
 // node_t* next = node->next;
  // TODO int val = next->val;
  if (node->val + next_val == 2) {
  //if (node->val + sum == 2) {
//        int a;
//        klee_make_symbolic(&a, sizeof(a), "int");
//        if (2 == a) {
          klee_warning("assert is about to be called");
          assert("lazy init should be utilized here");
        //}
    }
  return 0;
}


