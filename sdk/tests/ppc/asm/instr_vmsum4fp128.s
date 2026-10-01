test_vmsum4fp128_1:
  #_ REGISTER_IN v3 [3f800000, 3fc00000, 3f8ccccd, 3ff33333]
  #_ REGISTER_IN v4 [40000000, 40700000, 4013d70a, 40b051eb]
  vmsum4fp128 v5, v3, v4
  blr
  #_ REGISTER_OUT v3 [3f800000, 3fc00000, 3f8ccccd, 3ff33333]
  #_ REGISTER_OUT v4 [40000000, 40700000, 4013d70a, 40b051eb]
  #_ REGISTER_OUT v5 [41A5147B, 41A5147B, 41A5147B, 41A5147B]

test_vmsum4fp128_overflow:
  # Float32 overflow of a finite float64 sum gives QNaN (see instr_vmsum3fp128.s).
  #_ REGISTER_IN v3 [7f7fffff, 7f7fffff, 00000000, 00000000]
  #_ REGISTER_IN v4 [7f7fffff, 3f800000, 00000000, 00000000]
  vmsum4fp128 v5, v3, v4
  blr
  #_ REGISTER_OUT v5 [7FC00000, 7FC00000, 7FC00000, 7FC00000]