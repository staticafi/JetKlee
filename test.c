//
// Created by capso on 05.11.21.
//

#include <assert.h>
#include <stdlib.h>

typedef struct node {
  int val;
  struct node *next;
  char* str[5];
} node_t;

typedef struct {
  int val1;
  int val2;
} out_of_bounds_t;

//extern node_t* node;
extern int* val;

int get_sign(void* node_void, out_of_bounds_t* outOfBounds) {

  node_t* node = (node_t*)node_void;

  void* void_ptr = outOfBounds;
  int* pInt = (int*)void_ptr + 0;
  if (*pInt == 0) {
    if (*pInt != outOfBounds->val1) {
      assert(0 && "void ptr test failed");
    } else {
      klee_warning("void_ptr passed");
    }
  }


  int SIZE = 5;
  char str[SIZE];
  klee_make_symbolic(str, sizeof(str), "str_symbolic");
  //check for out of range
  if (node->str[SIZE + 1] == 'a') {
    assert(0 && "out of range!");
  } else {
    klee_warning("out of range passed");
  }

  node_t* next = node->next;
  while (next->next) {
    next = next->next;
    klee_warning("cycle continues!");
  }

  node_t* next2 = next;
  while (next2->next) {
    next2 = next2->next;
    klee_warning("cycle2 continues!");
  }
//
  node_t* next3 = node->next;
  while (next3->next) {
    next3 = next3->next;
    klee_warning("cycle3 continues!");
  }

  int* value_ptr = &node->val;
  if (*value_ptr == 1) {
      if (node->val != 1) {
        assert(0 && "values are not the same");
      } else {
        klee_warning("ptr to lazy initiated value check passed");
      }
  }

  //compare pointer values (bc thesis check) - both branches should be reachable
  int* ptr = (int*)malloc(sizeof(int));
  if (&node->next > ptr) {
    klee_warning("reachable");
  } else {
    klee_warning("also reachable");
  }

  //compare values in multiple reads
  if (*val == 1) {
    if (*val != 1) {
      assert("int* value eq check failed");
    }
    if (*val == 3) {
      assert(0 && "int* value neq check failed");
    }
    if (*val == 1) {
      klee_warning("int* tests passed");
    }
  }


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
  if (node->val == 3) {
    klee_warning("node_t* diff branch eq check passed!");
  }
//
  int* next_int = &node->next->val;
  if (node->next->val == 1) {
    if (*next_int != 1) {
      assert(0 && "node_t->next* value eq check failed");
    } else {
      klee_warning("node_t* double pointer check passed");
    }
    if (node->next->val == 3) {
      assert("node_t->next* value neq check failed");
    }
  }

  return 0;
}


