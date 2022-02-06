//
// Created by capso on 05.11.21.
//

#include <assert.h>

typedef struct node {
  int val;
  struct node *next;
  int* val2;
} node_t;

extern node_t* node;
extern int* val;

//int get_sign(node_t* node) {}

int main() {
  int* value_ptr = &node->val;
  if (*value_ptr == 1) {
    klee_warning("ptr to lazy initiated value check passed");
      if (node->val != 1) {
        assert(0 && "values are not the same");
      }
  }

  //compare pointer values (bc thesis check)
  int * ptr = (int*)malloc(sizeof(int));
  if (&node->next > ptr) {
    klee_warning("is true");
  } else {
    klee_warning("also true");
  }

  //compare values in multiple reads
  if (*val == 1) {
    if (*val != 1) {
      klee_warning("int* value eq check failed");
    }
    if (*val == 3) {
      assert(0 && "int* value neq check failed");
    }
  }
  klee_warning("int* tests passed");

  if (node->val == 1) {
    if (node->val == 1) {
      klee_warning("node_t* value eq check passed");
    } else {
      assert(0 && "node_t* value eq check failed");
    }
    if (node->val == 3) {
      assert(0 && "node_t* value neq check failed");
    }
  }
  klee_warning("node_t* tests passed");

  if (node->val == 3) {
    klee_warning("node_t* diff branch eq check passed!");
  }

  if (node->next->val == 1) {
    if (node->next->val != 1) {
      assert(0 && "node_t->next* value eq check failed");
    } else {
      klee_warning("node_t* double pointer check passed");
    }
    if (node->next->val == 3) {
      assert("node_t->next* value neq check failed");
    }
  }

  if(*node->val2 == 1) {
    if (*node->val2 == 1) {
      klee_warning("val2 eq passed");
    } else {
      assert(0 && "val2 eq failed");
    }
    if (*node->val2 == 3) {
      assert(0 && "val2 neq failed");
    }
  }

  //cap the cycle so the run is not infinite
//  while (node->next) {
//    klee_warning("try me!");
//  }
  return 0;
}


