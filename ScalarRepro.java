import java.util.ArrayList;
import java.util.SplittableRandom;
import java.util.concurrent.atomic.AtomicLong;

/**
 * Scalar reproducer, no incubator modules: a C2-compiled loop keeps 24 double accumulators and their 24
 * multipliers live across the loop's safepoint poll. With AVX-512 (UseAVX=3) C2 allocates doubles to
 * xmm0-xmm31, so some of them live in xmm16-xmm31 across the poll. The result is compared bit for bit
 * with the same arithmetic done through arrays (memory, not registers), so a register that does not
 * survive a safepoint shows up as a mismatch instead of a crash.
 *
 * <p>Variant {@code oop} additionally keeps a reference live across the loop (read only after it), which
 * C2 may spill to an XMM register (UseFPUForSpilling); if that register is lost the read crashes.
 *
 * <p>Usage: {@code java -XX:+UnlockDiagnosticVMOptions -XX:+SafepointALot -XX:GuaranteedSafepointInterval=1
 * ScalarRepro <doubles|oop> <seconds> <threads>}. Exit code 0 when every call matched, 3 on a mismatch.
 */
public final class ScalarRepro {
    static final class Holder {
        final double bias;

        Holder(double bias) {
            this.bias = bias;
        }
    }

    static double[] multipliers(double seed) {
        var m = new double[24];
        for (var i = 0; i < m.length; i++) {
            m[i] = 0.5 + seed * (i + 1) / 64.0;
        }
        return m;
    }

    /** Register version: 24 accumulators and 24 multipliers live across the loop. */
    static double registers(double[] x, double[] m, Holder h, boolean useHolder) {
        double m0 = m[0], m1 = m[1], m2 = m[2], m3 = m[3], m4 = m[4], m5 = m[5], m6 = m[6], m7 = m[7];
        double m8 = m[8], m9 = m[9], m10 = m[10], m11 = m[11], m12 = m[12], m13 = m[13], m14 = m[14], m15 = m[15];
        double m16 = m[16], m17 = m[17], m18 = m[18], m19 = m[19], m20 = m[20], m21 = m[21], m22 = m[22], m23 = m[23];
        double a0 = 0, a1 = 0, a2 = 0, a3 = 0, a4 = 0, a5 = 0, a6 = 0, a7 = 0;
        double a8 = 0, a9 = 0, a10 = 0, a11 = 0, a12 = 0, a13 = 0, a14 = 0, a15 = 0;
        double a16 = 0, a17 = 0, a18 = 0, a19 = 0, a20 = 0, a21 = 0, a22 = 0, a23 = 0;
        for (var i = 0; i < x.length; i++) {
            var v = x[i];
            a0 = a0 * m0 + v; a1 = a1 * m1 + v; a2 = a2 * m2 + v; a3 = a3 * m3 + v;
            a4 = a4 * m4 + v; a5 = a5 * m5 + v; a6 = a6 * m6 + v; a7 = a7 * m7 + v;
            a8 = a8 * m8 + v; a9 = a9 * m9 + v; a10 = a10 * m10 + v; a11 = a11 * m11 + v;
            a12 = a12 * m12 + v; a13 = a13 * m13 + v; a14 = a14 * m14 + v; a15 = a15 * m15 + v;
            a16 = a16 * m16 + v; a17 = a17 * m17 + v; a18 = a18 * m18 + v; a19 = a19 * m19 + v;
            a20 = a20 * m20 + v; a21 = a21 * m21 + v; a22 = a22 * m22 + v; a23 = a23 * m23 + v;
        }
        var sum = a0 + a1 + a2 + a3 + a4 + a5 + a6 + a7 + a8 + a9 + a10 + a11
                + a12 + a13 + a14 + a15 + a16 + a17 + a18 + a19 + a20 + a21 + a22 + a23;
        return useHolder ? sum + h.bias : sum;
    }

    /** Memory version of the same arithmetic, in the same order. */
    static double memory(double[] x, double[] m, Holder h, boolean useHolder) {
        var a = new double[24];
        for (var v : x) {
            for (var k = 0; k < 24; k++) {
                a[k] = a[k] * m[k] + v;
            }
        }
        var sum = a[0];
        for (var k = 1; k < 24; k++) {
            sum += a[k];
        }
        return useHolder ? sum + h.bias : sum;
    }

    public static void main(String[] args) throws InterruptedException {
        var variant = args.length > 0 ? args[0] : "doubles";
        var seconds = args.length > 1 ? Integer.parseInt(args[1]) : 30;
        var threads = args.length > 2 ? Integer.parseInt(args[2]) : 4;
        if (!variant.equals("doubles") && !variant.equals("oop")) {
            throw new IllegalArgumentException("variant: doubles or oop");
        }
        var useHolder = variant.equals("oop");
        System.out.println("ScalarRepro " + variant + ", " + seconds + " s, " + threads + " threads");
        var deadline = System.nanoTime() + seconds * 1_000_000_000L;
        var calls = new AtomicLong();
        var mismatches = new AtomicLong();
        var workers = new ArrayList<Thread>();
        for (var t = 0; t < threads; t++) {
            var seed = t;
            workers.add(Thread.ofPlatform().name("worker-" + t).start(() -> {
                var random = new SplittableRandom(seed);
                var x = new double[20_000 + seed];
                for (var i = 0; i < x.length; i++) {
                    x[i] = random.nextDouble(-1, 1);
                }
                var m = multipliers(0.25 + seed / 16.0);
                var h = new Holder(1.0 + seed);
                var expected = memory(x, m, h, useHolder);
                while (System.nanoTime() < deadline) {
                    for (var i = 0; i < 64; i++) {
                        var actual = registers(x, m, h, useHolder);
                        if (Double.doubleToRawLongBits(actual) != Double.doubleToRawLongBits(expected)
                                && mismatches.incrementAndGet() <= 5) {
                            System.out.println("mismatch in " + Thread.currentThread().getName() + ": " + actual
                                    + " != " + expected);
                        }
                    }
                    calls.addAndGet(64);
                }
            }));
        }
        // garbage so that collections keep stopping the workers in their loops
        Thread.ofPlatform().daemon().name("churn").start(() -> {
            var keep = new Object[64];
            var random = new SplittableRandom(99);
            while (true) {
                keep[random.nextInt(keep.length)] = new byte[random.nextInt(16, 65536)];
            }
        });
        for (var worker : workers) {
            worker.join();
        }
        System.out.println("calls " + calls.get() + ", mismatches " + mismatches.get());
        System.exit(mismatches.get() == 0 ? 0 : 3);
    }
}
