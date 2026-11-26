// xstatefill.c: the JNI half of XStateKernel.java. fillBelowStack() writes 0x8000000000000000 into the 32 KiB below
// the stack pointer of its Java caller, where the next exception context that frame takes is built. An affected
// Windows build trusts that value as the header of the context's extended-state area: on x64 it names no AVX
// component, and on Arm64 the pair {bit 63, bit 63} makes the context leave out the SVE state (native/sve_stale_header.c
// measures both). The same value as fillBelowStack() of the JDK test libExceptionXState.c (JDK-8365196).
// MIT License.
#include <jni.h>
#include <stdint.h>

#define FILL_QWORDS 4096 /* 32 KiB */

static volatile uint64_t sink;

JNIEXPORT void JNICALL Java_XStateKernel_fillBelowStack(JNIEnv* env, jclass cls) {
  volatile uint64_t buf[FILL_QWORDS];
  int i;
  (void)env;
  (void)cls;
  for (i = 0; i < FILL_QWORDS; i++) {
    buf[i] = 0x8000000000000000ULL;
  }
  sink = buf[FILL_QWORDS - 1];
}
