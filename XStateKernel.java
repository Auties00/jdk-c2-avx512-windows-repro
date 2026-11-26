import jdk.incubator.vector.DoubleVector;
import jdk.incubator.vector.LongVector;
import jdk.incubator.vector.VectorSpecies;

/**
 * The deterministic form of the JDK reproducer: the kernels of the JDK test TestExceptionXStateScrub (JDK-8365196) as a
 * standalone program. Each kernel calls native code that leaves the stale extended-state header below its stack
 * pointer (native/xstatefill.c) and then keeps eight vector accumulators of -0.0 live across the traps compiled code
 * takes: safepoint polls under SafepointALot, and an implicit null check that faults once every 64 calls. An upper
 * lane reset by an incomplete exception context becomes +0.0 and stays, so the AND of the lanes shows any loss.
 * kernel256 runs on AVX2 and on SVE with 256-bit vectors, kernel512 on AVX-512.
 *
 * <p>Usage: {@code java -Djava.library.path=<dir of xstatefill.dll> --add-modules jdk.incubator.vector XStateKernel
 * [seconds]} with the flags of the test (SafepointALot, GuaranteedSafepointInterval=1, the trap limits). Exit status 0
 * when no call lost a lane, 3 when one did.
 *
 * <p>MIT License.
 */
public class XStateKernel {
    static final int ITERS = 200_000;
    static final int NULL_PERIOD = 64;

    static volatile boolean stop;
    static Object sink;

    static {
        System.loadLibrary("xstatefill");
    }

    static native void fillBelowStack();

    static class Holder {
        int x = 1;
    }

    static final VectorSpecies<Double> S256 = DoubleVector.SPECIES_256;
    static final VectorSpecies<Double> S512 = DoubleVector.SPECIES_512;

    static long kernel256(double neg, Object[] refs, Holder[] holders) {
        fillBelowStack();
        DoubleVector v = DoubleVector.broadcast(S256, neg);
        DoubleVector a0 = DoubleVector.broadcast(S256, neg);
        DoubleVector a1 = DoubleVector.broadcast(S256, neg);
        DoubleVector a2 = DoubleVector.broadcast(S256, neg);
        DoubleVector a3 = DoubleVector.broadcast(S256, neg);
        DoubleVector a4 = DoubleVector.broadcast(S256, neg);
        DoubleVector a5 = DoubleVector.broadcast(S256, neg);
        DoubleVector a6 = DoubleVector.broadcast(S256, neg);
        DoubleVector a7 = DoubleVector.broadcast(S256, neg);
        int mask = refs.length - 1;
        int hmask = holders.length - 1;
        int count = 0;
        for (int i = 0; i < ITERS; i++) {
            if (refs[i & mask] != null) {
                count++;
            }
            try {
                count += holders[i & hmask].x;
            } catch (NullPointerException e) {
                count--;
            }
            a0 = a0.add(v);
            a1 = a1.add(v);
            a2 = a2.add(v);
            a3 = a3.add(v);
            a4 = a4.add(v);
            a5 = a5.add(v);
            a6 = a6.add(v);
            a7 = a7.add(v);
        }
        sink = count;
        return lostLanes(a0.reinterpretAsLongs().and(a1.reinterpretAsLongs()).and(a2.reinterpretAsLongs())
                .and(a3.reinterpretAsLongs()).and(a4.reinterpretAsLongs()).and(a5.reinterpretAsLongs())
                .and(a6.reinterpretAsLongs()).and(a7.reinterpretAsLongs()));
    }

    static long kernel512(double neg, Object[] refs, Holder[] holders) {
        fillBelowStack();
        DoubleVector v = DoubleVector.broadcast(S512, neg);
        DoubleVector a0 = DoubleVector.broadcast(S512, neg);
        DoubleVector a1 = DoubleVector.broadcast(S512, neg);
        DoubleVector a2 = DoubleVector.broadcast(S512, neg);
        DoubleVector a3 = DoubleVector.broadcast(S512, neg);
        DoubleVector a4 = DoubleVector.broadcast(S512, neg);
        DoubleVector a5 = DoubleVector.broadcast(S512, neg);
        DoubleVector a6 = DoubleVector.broadcast(S512, neg);
        DoubleVector a7 = DoubleVector.broadcast(S512, neg);
        int mask = refs.length - 1;
        int hmask = holders.length - 1;
        int count = 0;
        for (int i = 0; i < ITERS; i++) {
            if (refs[i & mask] != null) {
                count++;
            }
            try {
                count += holders[i & hmask].x;
            } catch (NullPointerException e) {
                count--;
            }
            a0 = a0.add(v);
            a1 = a1.add(v);
            a2 = a2.add(v);
            a3 = a3.add(v);
            a4 = a4.add(v);
            a5 = a5.add(v);
            a6 = a6.add(v);
            a7 = a7.add(v);
        }
        sink = count;
        return lostLanes(a0.reinterpretAsLongs().and(a1.reinterpretAsLongs()).and(a2.reinterpretAsLongs())
                .and(a3.reinterpretAsLongs()).and(a4.reinterpretAsLongs()).and(a5.reinterpretAsLongs())
                .and(a6.reinterpretAsLongs()).and(a7.reinterpretAsLongs()));
    }

    static long lostLanes(LongVector r) {
        long lost = 0;
        for (int l = 0; l < r.length(); l++) {
            if (r.lane(l) != Long.MIN_VALUE) {
                lost++;
            }
        }
        return lost;
    }

    public static void main(String[] args) {
        long seconds = args.length > 0 ? Long.parseLong(args[0]) : 10;
        Object[] refs = new Object[1 << 14];
        for (int i = 0; i < refs.length; i++) {
            refs[i] = new int[1 + (i & 7)];
        }
        Holder[] holders = new Holder[1 << 10];
        for (int i = 0; i < holders.length; i++) {
            holders[i] = new Holder();
        }
        Holder[] withNull = holders.clone();
        withNull[withNull.length / 2] = null;

        Thread gc = new Thread(() -> {
            while (!stop) {
                for (int i = 0; i < 2000; i++) {
                    sink = new byte[256];
                }
                System.gc();
                try {
                    Thread.sleep(2);
                } catch (InterruptedException e) {
                    return;
                }
            }
        });
        gc.setDaemon(true);
        gc.start();

        boolean has512 = DoubleVector.SPECIES_PREFERRED.vectorBitSize() >= 512;
        System.out.println("XStateKernel: preferred vector " + DoubleVector.SPECIES_PREFERRED.vectorBitSize()
                + " bits, kernel512 " + has512 + ", " + seconds + " s");
        long end = System.nanoTime() + seconds * 1_000_000_000L;
        long calls = 0;
        long wrong = 0;
        while (System.nanoTime() < end) {
            Holder[] h = (calls % NULL_PERIOD == NULL_PERIOD - 1) ? withNull : holders;
            long lost = kernel256(-0.0, refs, h);
            if (has512) {
                lost += kernel512(-0.0, refs, h);
            }
            calls++;
            if (lost != 0 && wrong++ < 5) {
                System.out.println("call " + calls + ": " + lost + " vector lanes lost");
            }
        }
        stop = true;
        System.out.println("calls " + calls + ", wrong results " + wrong);
        System.exit(wrong != 0 ? 3 : 0);
    }
}
