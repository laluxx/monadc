# monadc additional fuzz properties

This pack contains 152 new `.fuzz` property files for `tests/fuzzing/properties/`.

Design constraints:
- uses only the current `fuzz_codegen.py` schema: `name`, `section`, `args`, `type`, `expect`, `description`, `law`;
- uses only `Int` and `Bool` generated arguments;
- assumes only helpers already defined in `STABLE_DEFS` plus existing core forms/operators used by the current pack;
- avoids requiring new generators, arrays, strings, maps, sets, paths, or heap literals.

Files by section:

## arithmetic
- `tests/fuzzing/properties/int_add_cancellation_left_bool.fuzz` — Left-addition cancellation must preserve the truth value of equality.
- `tests/fuzzing/properties/int_add_cancellation_right_bool.fuzz` — Right-addition cancellation must preserve the truth value of equality.
- `tests/fuzzing/properties/int_add_four_way_regrouping.fuzz` — Addition of four generated values must be stable under regrouping and re-pairing.
- `tests/fuzzing/properties/int_add_identity_left_value.fuzz` — Adding zero on the left must preserve the generated Int expression exactly.
- `tests/fuzzing/properties/int_add_identity_right_value.fuzz` — Adding zero on the right must preserve the generated Int expression exactly.
- `tests/fuzzing/properties/int_add_inverse_left_zero.fuzz` — Adding an Int to its left-side additive inverse must produce zero.
- `tests/fuzzing/properties/int_add_inverse_right_zero.fuzz` — Adding an Int to its right-side additive inverse must produce zero.
- `tests/fuzzing/properties/int_add_same_offset_preserves_equality.fuzz` — Equality after adding the same offset to both operands must match equality before the offset.
- `tests/fuzzing/properties/int_add_sub_commuted_difference.fuzz` — Adding a difference must be stable when the outer addition is commuted.
- `tests/fuzzing/properties/int_add_sub_roundtrip_left_nested.fuzz` — Adding two values and subtracting one of them inside a larger expression must round-trip the other.
- `tests/fuzzing/properties/int_add_sub_roundtrip_right_nested.fuzz` — Nested addition followed by subtracting the preserved prefix must recover the final addend.
- `tests/fuzzing/properties/int_add_three_way_permutation.fuzz` — Addition of three generated values must be invariant under a nontrivial permutation.
- `tests/fuzzing/properties/int_double_negation.fuzz` — Negating an Int twice through subtraction from zero must recover the original.
- `tests/fuzzing/properties/int_mul_add_one_expansion.fuzz` — Multiplying by one plus a value must expand to the original value plus the product.
- `tests/fuzzing/properties/int_mul_annihilates_difference_self.fuzz` — Multiplying by a self-difference must produce zero.
- `tests/fuzzing/properties/int_mul_associative.fuzz` — Multiplication grouping must not affect generated Int results.
- `tests/fuzzing/properties/int_mul_by_three_expands_to_adds.fuzz` — Multiplication by three must agree with three repeated additions.
- `tests/fuzzing/properties/int_mul_by_two_equals_add_self.fuzz` — Multiplication by two must agree with adding a generated expression to itself.
- `tests/fuzzing/properties/int_mul_distributes_left_over_sub.fuzz` — A left-side factor must distribute over subtraction.
- `tests/fuzzing/properties/int_mul_distributes_right_over_add.fuzz` — A right-side factor must distribute over addition.
- `tests/fuzzing/properties/int_mul_distributes_right_over_sub.fuzz` — A right-side factor must distribute over subtraction.
- `tests/fuzzing/properties/int_mul_double_negative.fuzz` — Multiplying two negated operands must recover the positive product.
- `tests/fuzzing/properties/int_mul_factor_common_left.fuzz` — Common left factors in a sum of products must factor back out.
- `tests/fuzzing/properties/int_mul_factor_common_right.fuzz` — Common right factors in a sum of products must factor back out.
- `tests/fuzzing/properties/int_mul_four_way_regrouping.fuzz` — Multiplication of four generated values must be stable under regrouping.
- `tests/fuzzing/properties/int_mul_identity_left_value.fuzz` — Multiplying by one on the left must preserve the generated Int expression exactly.
- `tests/fuzzing/properties/int_mul_identity_right_value.fuzz` — Multiplying by one on the right must preserve the generated Int expression exactly.
- `tests/fuzzing/properties/int_mul_negative_left_equals_negative_right.fuzz` — Negating either multiplicand must produce the same negated product.
- `tests/fuzzing/properties/int_mul_negative_one_left.fuzz` — Multiplication by negative one on the left must match subtraction from zero.
- `tests/fuzzing/properties/int_mul_negative_one_right.fuzz` — Multiplication by negative one on the right must match subtraction from zero.
- `tests/fuzzing/properties/int_mul_sub_one_expansion.fuzz` — Multiplying by one less than a value must expand to the product minus the original value.
- `tests/fuzzing/properties/int_mul_subtract_self_zero.fuzz` — Subtracting identical generated products must produce zero.
- `tests/fuzzing/properties/int_mul_three_way_permutation.fuzz` — Multiplication of three generated values must be invariant under a nontrivial permutation.
- `tests/fuzzing/properties/int_mul_zero_left_value.fuzz` — Multiplying by zero on the left must produce zero.
- `tests/fuzzing/properties/int_mul_zero_right_value.fuzz` — Multiplying by zero on the right must produce zero.
- `tests/fuzzing/properties/int_negation_distributes_over_add.fuzz` — Negation must distribute over addition for generated Int expressions.
- `tests/fuzzing/properties/int_negation_reverses_subtraction.fuzz` — Negating a subtraction must reverse the operands.
- `tests/fuzzing/properties/int_sub_add_sub_pairing.fuzz` — A pair of subtractions added together must match subtracting the paired right operands from the paired left operands.
- `tests/fuzzing/properties/int_sub_as_add_negative.fuzz` — Subtraction must agree with adding the negated right operand.
- `tests/fuzzing/properties/int_sub_cancellation_left_equality.fuzz` — Subtracting two values from the same left operand must preserve equality of the right operands.
- `tests/fuzzing/properties/int_sub_cancellation_right_equality.fuzz` — Subtracting the same right operand from two values must preserve equality of the left operands.
- `tests/fuzzing/properties/int_sub_from_sum_reassociates.fuzz` — Subtracting from a sum must be equivalent to subtracting from one addend before adding.
- `tests/fuzzing/properties/int_sub_left_add_self_negative_rhs.fuzz` — Subtracting a value plus an offset from the value must leave the negated offset.
- `tests/fuzzing/properties/int_sub_negative_is_addition.fuzz` — Subtracting a negated generated Int must agree with addition.
- `tests/fuzzing/properties/int_sub_same_offset_preserves_difference.fuzz` — Adding the same offset to both sides of a subtraction must preserve the difference.
- `tests/fuzzing/properties/int_sub_self_after_add_zero.fuzz` — A generated expression minus itself after the same addition must still cancel to zero.
- `tests/fuzzing/properties/int_sub_sum_right_associates.fuzz` — Subtracting a sum on the right must match repeated subtraction.
- `tests/fuzzing/properties/int_sub_swapped_sum_negates_difference.fuzz` — The difference of swapped operands must equal the negated original difference.
- `tests/fuzzing/properties/int_sub_zero_left_negates_twice.fuzz` — The zero-left subtraction form must match explicit negative-one multiplication.
- `tests/fuzzing/properties/int_sub_zero_right_value.fuzz` — Subtracting zero on the right must preserve the generated Int expression.
- `tests/fuzzing/properties/int_zero_sub_as_negation.fuzz` — Subtracting an Int from zero must agree with multiplication by negative one.

## binding
- `tests/fuzzing/properties/with_binding_in_comparison.fuzz` — With-bound Int values must be usable in comparison helpers.
- `tests/fuzzing/properties/with_binding_in_conditional.fuzz` — With-bound values must preserve conditional behavior in a body.
- `tests/fuzzing/properties/with_binding_matches_max_definition.fuzz` — A with body using an if-based maximum must match the typed max helper.
- `tests/fuzzing/properties/with_binding_matches_min_definition.fuzz` — A with body using an if-based minimum must match the typed min helper.
- `tests/fuzzing/properties/with_binding_used_twice.fuzz` — A with-bound Int used twice must behave like using the original expression twice.
- `tests/fuzzing/properties/with_do_inner_shadowing_result.fuzz` — A do body inside with must return the inner shadowed binding when that is its final expression.
- `tests/fuzzing/properties/with_do_returns_outer_binding.fuzz` — A with-bound value must survive sequencing in a do body.
- `tests/fuzzing/properties/with_identity_binding_returns_value.fuzz` — A simple with binding must expose its bound Int value in the body.
- `tests/fuzzing/properties/with_if_branch_shadowing.fuzz` — A branch-local with binding must shadow only inside the selected branch.
- `tests/fuzzing/properties/with_nested_outer_visible_after_inner.fuzz` — After an inner shadowing binding ends, the outer binding must remain visible in the surrounding body.
- `tests/fuzzing/properties/with_shadowing_bool_condition.fuzz` — A with-bound Bool condition must choose the same branch as the original Bool expression.
- `tests/fuzzing/properties/with_two_bindings_multiplication.fuzz` — Two with-bound Int values must preserve multiplication semantics.
- `tests/fuzzing/properties/with_two_bindings_subtraction.fuzz` — Two with-bound Int values must preserve subtraction semantics.

## comparison
- `tests/fuzzing/properties/comparison_add_preserves_lt.fuzz` — Adding the same generated offset must preserve strict less-than ordering.
- `tests/fuzzing/properties/comparison_add_preserves_lte.fuzz` — Adding the same generated offset must preserve less-or-equal ordering.
- `tests/fuzzing/properties/comparison_eq_implies_gte.fuzz` — Integer equality must imply greater-or-equal in both directions.
- `tests/fuzzing/properties/comparison_eq_implies_lte.fuzz` — Integer equality must imply less-or-equal in both directions.
- `tests/fuzzing/properties/comparison_greater_than_predecessor.fuzz` — An Int expression must be greater than itself minus one.
- `tests/fuzzing/properties/comparison_gt_implies_gte.fuzz` — Strict greater-than must imply greater-or-equal.
- `tests/fuzzing/properties/comparison_gt_implies_not_equal.fuzz` — Strict greater-than must imply integer inequality.
- `tests/fuzzing/properties/comparison_gt_is_not_lte_encoded.fuzz` — Greater-than must be the encoded negation of less-or-equal.
- `tests/fuzzing/properties/comparison_gt_transitive.fuzz` — Strict greater-than must be transitive.
- `tests/fuzzing/properties/comparison_gte_antisymmetric.fuzz` — If two values are greater-or-equal in both directions, they must be equal.
- `tests/fuzzing/properties/comparison_gte_as_gt_or_eq_encoded.fuzz` — Greater-or-equal must agree with the encoded greater-than-or-equality definition.
- `tests/fuzzing/properties/comparison_gte_self_true.fuzz` — Greater-or-equal must be true for an Int expression compared to itself.
- `tests/fuzzing/properties/comparison_gte_totality_encoded.fuzz` — For any two Int expressions, at least one greater-or-equal direction must hold.
- `tests/fuzzing/properties/comparison_gte_transitive.fuzz` — Greater-or-equal must be transitive.
- `tests/fuzzing/properties/comparison_less_than_successor.fuzz` — An Int expression must be less than itself plus one.
- `tests/fuzzing/properties/comparison_lt_implies_lte.fuzz` — Strict less-than must imply less-or-equal.
- `tests/fuzzing/properties/comparison_lt_implies_not_equal.fuzz` — Strict less-than must imply integer inequality.
- `tests/fuzzing/properties/comparison_lt_is_not_gte_encoded.fuzz` — Less-than must be the encoded negation of greater-or-equal.
- `tests/fuzzing/properties/comparison_lt_transitive.fuzz` — Strict less-than must be transitive.
- `tests/fuzzing/properties/comparison_lte_antisymmetric.fuzz` — If two values are less-or-equal in both directions, they must be equal.
- `tests/fuzzing/properties/comparison_lte_as_lt_or_eq_encoded.fuzz` — Less-or-equal must agree with the encoded less-than-or-equality definition.
- `tests/fuzzing/properties/comparison_lte_self_true.fuzz` — Less-or-equal must be true for an Int expression compared to itself.
- `tests/fuzzing/properties/comparison_lte_totality_encoded.fuzz` — For any two Int expressions, at least one less-or-equal direction must hold.
- `tests/fuzzing/properties/comparison_lte_transitive.fuzz` — Less-or-equal must be transitive.
- `tests/fuzzing/properties/comparison_negation_reverses_lt.fuzz` — Negating both operands must reverse strict less-than ordering.
- `tests/fuzzing/properties/comparison_negation_reverses_lte.fuzz` — Negating both operands must reverse less-or-equal ordering.
- `tests/fuzzing/properties/comparison_sub_preserves_lte_same_rhs.fuzz` — Subtracting the same right operand must preserve less-or-equal ordering.
- `tests/fuzzing/properties/comparison_trichotomy_encoded.fuzz` — Exactly one of less-than, greater-than, or equality must explain a pair of Int expressions.

## control
- `tests/fuzzing/properties/do_bool_returns_last_after_int_first.fuzz` — A do expression with an Int first expression must return the final Bool expression.
- `tests/fuzzing/properties/do_first_does_not_change_comparison.fuzz` — The first expression in do must not alter the comparison returned by the final expression.
- `tests/fuzzing/properties/do_if_last_returns_branch.fuzz` — A do expression whose last expression is an if must return the selected if branch.
- `tests/fuzzing/properties/do_int_returns_last_after_bool_first.fuzz` — A do expression with a Bool first expression must return the final Int expression.
- `tests/fuzzing/properties/do_nested_mixed_returns_last_bool.fuzz` — Nested mixed-type do sequencing must return the final Bool expression.
- `tests/fuzzing/properties/do_true_literal_returns_true.fuzz` — A do expression ending in literal True must return True even after evaluating an Int first expression.
- `tests/fuzzing/properties/if_bool_identity_branches_roundtrip.fuzz` — Choosing between typed Bool identity branches must preserve the condition.
- `tests/fuzzing/properties/if_bool_true_false_roundtrip.fuzz` — Choosing between True and False with a Bool condition must return that condition.
- `tests/fuzzing/properties/if_branch_result_can_be_bool.fuzz` — A Bool-returning if expression must be unaffected by passing its condition through the typed Bool identity helper.
- `tests/fuzzing/properties/if_distributes_over_add_left.fuzz` — Adding a common left operand outside an if must match adding it inside each branch.
- `tests/fuzzing/properties/if_distributes_over_add_right.fuzz` — Adding a common right operand outside an if must match adding it inside each branch.
- `tests/fuzzing/properties/if_distributes_over_mul_right.fuzz` — Multiplying an if result by a common right operand must match multiplication inside both branches.
- `tests/fuzzing/properties/if_distributes_over_sub_left.fuzz` — Subtracting a common right operand from an if result must match subtraction inside both branches.
- `tests/fuzzing/properties/if_false_returns_else_int.fuzz` — An if expression with literal False must return the else branch.
- `tests/fuzzing/properties/if_nested_same_condition_else.fuzz` — A nested if in the else branch with the same condition must simplify to the outer branch choice.
- `tests/fuzzing/properties/if_nested_same_condition_then.fuzz` — A nested if in the then branch with the same condition must simplify to the outer branch choice.
- `tests/fuzzing/properties/if_order_partition_lte.fuzz` — An if over less-or-equal must select a value that remains less-or-equal to the other branch boundary.
- `tests/fuzzing/properties/if_true_returns_then_int.fuzz` — An if expression with literal True must return the then branch.

## function
- `tests/fuzzing/properties/bool_identity_preserves_if_condition.fuzz` — Passing a generated Bool through the typed Bool identity must not change conditional branch selection.
- `tests/fuzzing/properties/function_add_helper_matches_builtin.fuzz` — The typed fuzz-add helper must agree with the builtin addition operator.
- `tests/fuzzing/properties/function_bool_idempotent_on_comparison.fuzz` — The typed Bool identity helper must be idempotent on generated comparison results.
- `tests/fuzzing/properties/function_eq_helper_matches_builtin.fuzz` — The typed fuzz-eq-int? helper must agree with the builtin equality operator.
- `tests/fuzzing/properties/function_gt_helper_matches_builtin.fuzz` — The typed fuzz-gt? helper must agree with the builtin greater-than operator.
- `tests/fuzzing/properties/function_gte_helper_matches_builtin.fuzz` — The typed fuzz-gte? helper must agree with the builtin greater-or-equal operator.
- `tests/fuzzing/properties/function_int_id_preserves_addition.fuzz` — The typed Int identity helper must preserve an addition expression and its operands.
- `tests/fuzzing/properties/function_int_id_preserves_multiplication.fuzz` — The typed Int identity helper must preserve a multiplication expression and its operands.
- `tests/fuzzing/properties/function_lt_helper_matches_builtin.fuzz` — The typed fuzz-lt? helper must agree with the builtin less-than operator.
- `tests/fuzzing/properties/function_lte_helper_matches_builtin.fuzz` — The typed fuzz-lte? helper must agree with the builtin less-or-equal operator.
- `tests/fuzzing/properties/function_mul_helper_matches_builtin.fuzz` — The typed fuzz-mul helper must agree with the builtin multiplication operator.
- `tests/fuzzing/properties/function_sub_helper_matches_builtin.fuzz` — The typed fuzz-sub helper must agree with the builtin subtraction operator.

## negative
- `tests/fuzzing/properties/comparison_neq_self_false.fuzz` — Integer inequality of an expression against itself must be false.
- `tests/fuzzing/properties/comparison_predecessor_not_greater_than_self.fuzz` — A generated Int minus one must not be greater than the original Int.
- `tests/fuzzing/properties/comparison_successor_not_less_than_self.fuzz` — A generated Int plus one must not be less than the original Int.
- `tests/fuzzing/properties/do_false_literal_returns_false.fuzz` — A do expression ending in literal False must return False even after evaluating an Int first expression.
- `tests/fuzzing/properties/max_not_strictly_above_self.fuzz` — The max of an Int with itself must not be strictly above that Int.
- `tests/fuzzing/properties/min_not_strictly_below_self.fuzz` — The min of an Int with itself must not be strictly below that Int.

## order
- `tests/fuzzing/properties/max_absorbs_min_right.fuzz` — Max of a value and the min of that value with another must recover the value.
- `tests/fuzzing/properties/max_associative.fuzz` — The typed max helper must be associative.
- `tests/fuzzing/properties/max_bounded_by_left_and_right.fuzz` — The max helper result must be above both operands.
- `tests/fuzzing/properties/max_matches_if_definition.fuzz` — The typed max helper must match its direct conditional definition.
- `tests/fuzzing/properties/max_min_middle_right_absorption.fuzz` — Min of max(a,b) and b must recover b.
- `tests/fuzzing/properties/max_monotone_left_lte.fuzz` — Max must be monotone in its left operand under less-or-equal.
- `tests/fuzzing/properties/max_monotone_right_lte.fuzz` — Max must be monotone in its right operand under less-or-equal.
- `tests/fuzzing/properties/max_of_predecessor_is_original.fuzz` — The max of an Int and its predecessor must be the original Int.
- `tests/fuzzing/properties/max_respects_gte_left.fuzz` — If the left value is greater-or-equal, max must return the left value.
- `tests/fuzzing/properties/max_respects_gte_right.fuzz` — If the right value is greater-or-equal, max must return the right value.
- `tests/fuzzing/properties/max_with_upper_operand_after_offset.fuzz` — If b is above a, adding the same offset must make max choose the offset b side.
- `tests/fuzzing/properties/min_absorbs_max_right.fuzz` — Min of a value and the max of that value with another must recover the value.
- `tests/fuzzing/properties/min_associative.fuzz` — The typed min helper must be associative.
- `tests/fuzzing/properties/min_bounded_by_left_and_right.fuzz` — The min helper result must be below both operands.
- `tests/fuzzing/properties/min_lte_max.fuzz` — The minimum of two generated values must be less than or equal to their maximum.
- `tests/fuzzing/properties/min_matches_if_definition.fuzz` — The typed min helper must match its direct conditional definition.
- `tests/fuzzing/properties/min_max_middle_right_absorption.fuzz` — Max of min(a,b) and b must recover b.
- `tests/fuzzing/properties/min_max_sum_decomposition.fuzz` — The sum of min and max must equal the sum of the original operands.
- `tests/fuzzing/properties/min_monotone_left_lte.fuzz` — Min must be monotone in its left operand under less-or-equal.
- `tests/fuzzing/properties/min_monotone_right_lte.fuzz` — Min must be monotone in its right operand under less-or-equal.
- `tests/fuzzing/properties/min_of_successor_is_original.fuzz` — The min of an Int and its successor must be the original Int.
- `tests/fuzzing/properties/min_respects_lte_left.fuzz` — If the left value is less-or-equal, min must return the left value.
- `tests/fuzzing/properties/min_respects_lte_right.fuzz` — If the right value is less-or-equal, min must return the right value.
- `tests/fuzzing/properties/min_with_lower_operand_after_offset.fuzz` — If a is below b, adding the same offset must make min choose the offset a side.
