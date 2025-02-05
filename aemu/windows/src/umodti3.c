//===-- umodti3.c - Implement __umodti3 -----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements __umodti3 for the compiler_rt library.
//
//===----------------------------------------------------------------------===//

#include "int_lib.h"

#ifdef CRT_HAS_128BIT

// Returns: a % b

COMPILER_RT_ABI tu_int __umodti3(tu_int a, tu_int b) {
  tu_int r;
  __udivmodti4(a, b, &r);
  return r;
}

#endif // CRT_HAS_128BIT