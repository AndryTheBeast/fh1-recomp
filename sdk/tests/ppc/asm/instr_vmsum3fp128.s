test_vmsum3fp128_1:
  # v3 = [1.0,  1.5,  1.1, 0.0]
  # v4 = [2.0, 3.75, 2.31, 0.0]
  #_ REGISTER_IN v3 [3f800000, 3fc00000, 3f8ccccd, 01020304]
  #_ REGISTER_IN v4 [40000000, 40700000, 4013d70a, 01020304]
  vmsum3fp128 v5, v3, v4
  blr
  #_ REGISTER_OUT v3 [3f800000, 3fc00000, 3f8ccccd, 01020304]
  #_ REGISTER_OUT v4 [40000000, 40700000, 4013d70a, 01020304]
  #_ REGISTER_OUT v5 [4122A7F0, 4122A7F0, 4122A7F0, 4122A7F0]

test_vmsum3fp128_overflow:
  # |(-FLT_MAX, -FLT_MAX, -FLT_MAX)|^2 overflows float32: the console (and Xenia) give QNaN, not
  # +inf. FH1's GJK depends on it (empty simplex = -FLT_MAX, progress test needs NaN).
  #_ REGISTER_IN v3 [ff7fffff, ff7fffff, ff7fffff, ff7fffff]
  vmsum3fp128 v5, v3, v3
  blr
  #_ REGISTER_OUT v5 [7FC00000, 7FC00000, 7FC00000, 7FC00000]

test_vmsum3fp128_inf_input:
  # An infinity in the inputs is not an overflow: it passes through.
  #_ REGISTER_IN v3 [7f800000, 3f800000, 3f800000, 00000000]
  #_ REGISTER_IN v4 [3f800000, 3f800000, 3f800000, 00000000]
  vmsum3fp128 v5, v3, v4
  blr
  #_ REGISTER_OUT v5 [7F800000, 7F800000, 7F800000, 7F800000]