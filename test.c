//
// Created by capso on 05.11.21.
//

#include <assert.h>

typedef struct node {
  int val;
  struct node *next;
//  int val; //Switch with the other val to compare 0 or 8 offsets


} node_t;

int get_sign(node_t* node) {
  if (node->val == 1) {
    if (node->val == 1) {
      klee_warning("Should not be reachable");
    }
  }
//  if (node->val == 1) {
//    klee_warning("this warning should be reachable");
//    if (node->val == 1) {
//      assert(0 && "this should be an error");
//    }
//  }
//  if (node->next->val == 1) {
//    klee_warning("this warning should be reachable");
//    if (node->next->val == 3) {
//      assert(0 && "this should also be an error");
//    }
//  }
}


