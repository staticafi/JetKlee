#include "gtest/gtest.h"

#include "klee/Expr/ArrayCache.h"
#include "klee/Expr/Assignment.h"

#include <iostream>
#include <vector>

int finished = 0;

using namespace klee;

TEST(AssignmentTest, FoldNotOptimized)
{
  ArrayCache ac;
  const Array* array = ac.CreateArray("simple_array", /*size=*/ 1);
  // Create a simple assignment
  std::vector<const Array*> objects;
  std::vector<unsigned char> value;
  std::vector< std::vector<unsigned char> > values;
  objects.push_back(array);
  value.push_back(128);
  values.push_back(value);
  // We want to simplify to a constant so allow free values so
  // if the assignment is incomplete we don't get back a constant.
  VectorAssignment assignment(objects, values, /*_allowFreeValues=*/true);

  // Now make an expression that reads from the array at position
  // zero.
  ref<Expr> read = NotOptimizedExpr::alloc(Expr::createTempRead(array, Expr::Int8));

  // Now evaluate. The OptimizedExpr should be folded
  ref<Expr> evaluated = assignment.evaluate(read);
  const ConstantExpr* asConstant = dyn_cast<ConstantExpr>(evaluated);
  ASSERT_TRUE(asConstant != NULL);
  ASSERT_EQ(asConstant->getZExtValue(), (unsigned) 128);
}

TEST(AssignmentTest, CompactModel)
{
  ArrayCache ac;
  const Array* array = ac.CreateArray("simple_array", /*size=*/ 1);
  Assignment::map_bindings_ty values;
  MapArrayModel &model = values[array];
  model.add(4, 8);
  model.add(5, 16);
  model.add(7, 17);
  model.add(320000, 32);
  Assignment a(values);
  ASSERT_EQ(a.getValue(array, 0), 0);
  ASSERT_EQ(a.getValue(array, 4), 8);
  ASSERT_EQ(a.getValue(array, 5), 16);
  ASSERT_EQ(a.getValue(array, 6), 0);
  ASSERT_EQ(a.getValue(array, 7), 17);
  ASSERT_EQ(a.getValue(array, 100), 0);
  ASSERT_EQ(a.getValue(array, 320000), 32);
  CompactArrayModel& compactModel = a.getBindings(array);
  MapArrayModel(a.getBindings(array)).toCompact(compactModel);
  ASSERT_EQ(a.getValue(array, 0), 0);
  ASSERT_EQ(a.getValue(array, 4), 8);
  ASSERT_EQ(a.getValue(array, 5), 16);
  ASSERT_EQ(a.getValue(array, 6), 0);
  ASSERT_EQ(a.getValue(array, 7), 17);
  ASSERT_EQ(a.getValue(array, 100), 0);
  ASSERT_EQ(a.getValue(array, 320000), 32);
}

TEST(AssignmentTest, CompactModelAsVector)
{
  ArrayCache ac;
  const Array* array = ac.CreateArray("simple_array", /*size=*/ 1);
  Assignment::map_bindings_ty values;
  MapArrayModel &model = values[array];
  model.add(4, 8);
  model.add(5, 16);
  model.add(7, 17);
  model.add(32, 32);
  Assignment a(values);
  std::vector<uint8_t> vec = a.getBindings(array).asVector();
  ASSERT_EQ(vec.size(), 33u);
  ASSERT_EQ(vec[0], 0);
  ASSERT_EQ(vec[1], 0);
  ASSERT_EQ(vec[4], 8);
  ASSERT_EQ(vec[5], 16);
  ASSERT_EQ(vec[6], 0);
  ASSERT_EQ(vec[7], 17);
  ASSERT_EQ(vec[31], 0);
  ASSERT_EQ(vec[32], 32);
}
