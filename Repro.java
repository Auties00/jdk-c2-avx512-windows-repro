import jdk.incubator.vector.DoubleVector;
import jdk.incubator.vector.ByteVector;
import jdk.incubator.vector.IntVector;
import jdk.incubator.vector.VectorOperators;
import jdk.incubator.vector.VectorSpecies;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.SplittableRandom;
import java.util.concurrent.atomic.AtomicLong;

/**
 * JDK-8365196 reproducer: C2-compiled code crashes or computes wrong results when Windows loses vector register
 * state while the JVM handles the trap of a safepoint poll.
 *
 * <p>{@link #viaField} runs a Vector API loop and then calls through an instance field. With AVX-512, C2 keeps
 * values that are live across the loop's safepoint poll in the upper register bank (xmm16-xmm31). On an affected
 * Windows kernel those registers read back as zero after the poll traps:
 * <ul>
 *   <li>with the default flags, C2 may keep a reference there, which reloads as null: the JVM crashes with
 *       {@code EXCEPTION_ACCESS_VIOLATION} near address 0;</li>
 *   <li>with {@code -XX:-UseFPUForSpilling}, only data is kept there, and the output silently differs from the
 *       scalar result.</li>
 * </ul>
 *
 * <p>Usage: {@code java <flags> --add-modules jdk.incubator.vector -cp <classes> Repro [seconds] [threads]}. The
 * flags that make the trap frequent and fix the compiled code's register allocation are in the README. Every
 * call's output is compared with the scalar result. Exit status: 0 when every result was correct, 3 when one was
 * wrong; a crash writes an {@code hs_err} file instead.
 */
public final class Repro {
    private static final VectorSpecies<Integer> I4 = IntVector.SPECIES_128;

    private static final VectorSpecies<Byte> BP4 = ByteVector.SPECIES_128;

    private static final VectorSpecies<Double> D4 = DoubleVector.SPECIES_PREFERRED.length() >= 4 ? DoubleVector.SPECIES_256 : null;

    private static final Class<IntVector> INT4_VECTOR = vectorClass(I4);

    private static final Class<ByteVector> PIXEL4_VECTOR = vectorClass(BP4);

    private static final Class<DoubleVector> DOUBLE_VECTOR = D4 == null ? null : vectorClass(D4);

    private static final IntVector ZERO4 = IntVector.zero(I4);

    private static final IntVector MAX_SAMPLE4 = IntVector.broadcast(I4, 255);

    private static final double QUOTIENT_MARGIN = 0x1p-40;

    /** Reached through a field after the loop, so that a reference stays live across the loop's safepoint poll. */
    private final Tail tail = new Tail();

    @SuppressWarnings("unchecked")
    private static <E, V> Class<V> vectorClass(VectorSpecies<E> species) {
        return (Class<V>) species.vectorType();
    }

    /**
     * The scalar version of the computation, which is also the reference result: un-premultiplies planar RGBA
     * samples (four planes of {@code width} values, scaled by {@code scale}) into interleaved 8-bit RGBA.
     */
    static final class Tail {
        void range(int[] src, int sOff, int width, int scale, byte[] dst, int dOff, int from, int to) {
            rangeStatic(src, sOff, width, scale, dst, dOff, from, to);
        }

        static void rangeStatic(int[] src, int sOff, int width, int scale, byte[] dst, int dOff, int from, int to) {
            var alphaDivisor = 510L * scale;
            for (var x = from; x < to; x++) {
                var out = dOff + 4 * x;
                long a = src[sOff + 3 * width + x];
                if (a <= 0) {
                    dst[out] = 0;
                    dst[out + 1] = 0;
                    dst[out + 2] = 0;
                    dst[out + 3] = 0;
                    continue;
                }
                dst[out] = (byte) divide(src[sOff + x], a);
                dst[out + 1] = (byte) divide(src[sOff + width + x], a);
                dst[out + 2] = (byte) divide(src[sOff + 2 * width + x], a);
                dst[out + 3] = (byte) Math.min(255, (2 * a + 255L * scale) / alphaDivisor);
            }
        }

        private static int divide(int premultiplied, long alpha) {
            var quotient = Math.floorDiv(510L * premultiplied + alpha, 2 * alpha);
            return (int) Math.clamp(quotient, 0, 255);
        }
    }

    /**
     * The vector loop, four pixels at a time, followed by the tail through {@link #tail}. With AVX-512, C2 keeps
     * values that are live across the loop's safepoint poll in xmm16-xmm31, among them the {@code this} or
     * {@code tail} reference the call after the loop needs. The method body is unchanged since the StressSeed
     * values in the README were found: changing it changes the compiled code they select.
     */
    void viaField(int[] src, int sOff, int width, int scale, byte[] dst, int dOff) {
        var x = 0;
        if (D4 != null) {
            var alphaFactor = DOUBLE_VECTOR.cast(DoubleVector.broadcast(D4, 1.0 / (510.0 * scale)));
            var alphaBias = DOUBLE_VECTOR.cast(DoubleVector.broadcast(D4, 255.0 * scale));
            var twice = DOUBLE_VECTOR.cast(DoubleVector.broadcast(D4, 2.0));
            var colorScale = DOUBLE_VECTOR.cast(DoubleVector.broadcast(D4, 510.0));
            var half = DOUBLE_VECTOR.cast(DoubleVector.broadcast(D4, 0.5));
            var margin = DOUBLE_VECTOR.cast(DoubleVector.broadcast(D4, QUOTIENT_MARGIN));
            var low = DOUBLE_VECTOR.cast(DoubleVector.broadcast(D4, -1.0));
            var high = DOUBLE_VECTOR.cast(DoubleVector.broadcast(D4, 256.0));
            var zero = DOUBLE_VECTOR.cast(DoubleVector.zero(D4));
            for (; x + 4 <= width; x += 4) {
                var a = DOUBLE_VECTOR.cast(INT4_VECTOR.cast(IntVector.fromArray(I4, src, sOff + 3 * width + x))
                        .convertShape(VectorOperators.I2D, D4, 0));
                var red = DOUBLE_VECTOR.cast(INT4_VECTOR.cast(IntVector.fromArray(I4, src, sOff + x))
                        .convertShape(VectorOperators.I2D, D4, 0));
                var green = DOUBLE_VECTOR.cast(INT4_VECTOR.cast(IntVector.fromArray(I4, src, sOff + width + x))
                        .convertShape(VectorOperators.I2D, D4, 0));
                var blue = DOUBLE_VECTOR.cast(INT4_VECTOR.cast(IntVector.fromArray(I4, src, sOff + 2 * width + x))
                        .convertShape(VectorOperators.I2D, D4, 0));
                var reciprocal = DOUBLE_VECTOR.cast(half).lanewise(VectorOperators.DIV, a);
                var r = INT4_VECTOR.cast(red.lanewise(VectorOperators.MUL, colorScale).lanewise(VectorOperators.ADD, a)
                                .lanewise(VectorOperators.MUL, reciprocal).lanewise(VectorOperators.ADD, margin)
                                .lanewise(VectorOperators.MAX, low).lanewise(VectorOperators.MIN, high)
                                .convertShape(VectorOperators.D2I, I4, 0))
                        .lanewise(VectorOperators.MAX, ZERO4).lanewise(VectorOperators.MIN, MAX_SAMPLE4);
                var g = INT4_VECTOR.cast(green.lanewise(VectorOperators.MUL, colorScale)
                        .lanewise(VectorOperators.ADD, a)
                                .lanewise(VectorOperators.MUL, reciprocal).lanewise(VectorOperators.ADD, margin)
                                .lanewise(VectorOperators.MAX, low).lanewise(VectorOperators.MIN, high)
                                .convertShape(VectorOperators.D2I, I4, 0))
                        .lanewise(VectorOperators.MAX, ZERO4).lanewise(VectorOperators.MIN, MAX_SAMPLE4);
                var b = INT4_VECTOR.cast(blue.lanewise(VectorOperators.MUL, colorScale)
                        .lanewise(VectorOperators.ADD, a)
                                .lanewise(VectorOperators.MUL, reciprocal).lanewise(VectorOperators.ADD, margin)
                                .lanewise(VectorOperators.MAX, low).lanewise(VectorOperators.MIN, high)
                                .convertShape(VectorOperators.D2I, I4, 0))
                        .lanewise(VectorOperators.MAX, ZERO4).lanewise(VectorOperators.MIN, MAX_SAMPLE4);
                var alpha = INT4_VECTOR.cast(a.lanewise(VectorOperators.MUL, twice)
                        .lanewise(VectorOperators.ADD, alphaBias)
                                .lanewise(VectorOperators.MUL, alphaFactor).lanewise(VectorOperators.ADD, margin)
                                .convertShape(VectorOperators.D2I, I4, 0))
                        .lanewise(VectorOperators.MIN, MAX_SAMPLE4);
                var transparent = a.compare(VectorOperators.LE, zero).cast(I4);
                var packed = r.lanewise(VectorOperators.OR, INT4_VECTOR.cast(g).lanewise(VectorOperators.LSHL, 8))
                        .lanewise(VectorOperators.OR, INT4_VECTOR.cast(b).lanewise(VectorOperators.LSHL, 16))
                        .lanewise(VectorOperators.OR, INT4_VECTOR.cast(alpha).lanewise(VectorOperators.LSHL, 24))
                        .blend(ZERO4, transparent);
                PIXEL4_VECTOR.cast(INT4_VECTOR.cast(packed).reinterpretShape(BP4, 0)).intoArray(dst, dOff + 4 * x);
            }
        }
        tail.range(src, sOff, width, scale, dst, dOff, x, width);
    }

    public static void main(String[] args) throws InterruptedException {
        var seconds = args.length > 0 ? Integer.parseInt(args[0]) : 20;
        var threads = args.length > 1 ? Integer.parseInt(args[1]) : 4;
        System.out.println("Repro: " + seconds + " s, " + threads + " threads, java " + System.getProperty("java.vm.version")
                + ", preferred species " + DoubleVector.SPECIES_PREFERRED);
        var deadline = System.nanoTime() + seconds * 1_000_000_000L;
        var calls = new AtomicLong();
        var wrong = new AtomicLong();
        var workers = new ArrayList<Thread>();
        for (var t = 0; t < threads; t++) {
            var seed = t;
            workers.add(Thread.ofPlatform().name("repro-" + t).start(() -> {
                var random = new SplittableRandom(seed);
                var kernel = new Repro();
                // a width that is not a multiple of 4 leaves a tail, so the call after the loop always runs
                var width = 911 + 4 * seed;
                var scale = 1 << 14;
                var src = new int[4 * width];
                for (var x = 0; x < width; x++) {
                    var a = random.nextInt(-2, 256) * scale;
                    src[3 * width + x] = a;
                    for (var c = 0; c < 3; c++) {
                        src[c * width + x] = a <= 0 ? 0 : random.nextInt(0, a / 255 + 1) * 255 + random.nextInt(0, 255);
                    }
                }
                var expected = new byte[4 * width];
                Tail.rangeStatic(src, 0, width, scale, expected, 0, 0, width);
                var dst = new byte[4 * width];
                while (System.nanoTime() < deadline) {
                    for (var i = 0; i < 256; i++) {
                        kernel.viaField(src, 0, width, scale, dst, 0);
                        if (!Arrays.equals(dst, expected) && wrong.incrementAndGet() <= 5) {
                            System.out.println("wrong result at byte " + Arrays.mismatch(dst, expected) + " in "
                                    + Thread.currentThread().getName());
                        }
                        Arrays.fill(dst, (byte) 0x5A);
                    }
                    calls.addAndGet(256);
                }
            }));
        }
        // allocation keeps young collections, and so safepoints, frequent while the workers run
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
        System.out.println("calls " + calls.get() + ", wrong results " + wrong.get());
        System.out.println(wrong.get() == 0 ? "RESULT: every result was correct" : "RESULT: WRONG RESULTS");
        System.exit(wrong.get() == 0 ? 0 : 3);
    }
}
