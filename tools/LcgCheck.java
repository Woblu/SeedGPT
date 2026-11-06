// The reference side of tools/lcgcheck.c: the same table, from the real
// java.util.Random. test.sh diffs the two outputs. The internal state is read
// by reflection (Random.seed), because nothing else exposes it.
import java.lang.reflect.Field;
import java.util.Random;
import java.util.concurrent.atomic.AtomicLong;

public class LcgCheck {
    static long x;
    static long sm() {              // splitmix64, identical to lcgcheck.c
        long z = (x += 0x9E3779B97F4A7C15L);
        z = (z ^ (z >>> 30)) * 0xBF58476D1CE4E5B9L;
        z = (z ^ (z >>> 27)) * 0x94D049BB133111EBL;
        return z ^ (z >>> 31);
    }

    public static void main(String[] a) throws Exception {
        long[] fixed = { 0, 1, -1, 12345, 1L << 47, 0x5DEECE66DL, -4172144997902289642L };
        int[] bounds = { 1, 2, 3, 7, 16, 100, 1 << 30, (1 << 30) + 1, 0x7fffffff };
        Field f = Random.class.getDeclaredField("seed");
        f.setAccessible(true);
        x = 42;
        StringBuilder out = new StringBuilder();
        for (int i = 0; i < 7 + 200; i++) {
            long seed = i < 7 ? fixed[i] : sm();
            Random r = new Random(seed);
            out.append(seed).append(' ').append(r.nextInt());
            for (int b : bounds) out.append(' ').append(r.nextInt(b));
            out.append(' ').append(r.nextLong());
            long state = ((AtomicLong) f.get(r)).get();
            out.append(' ').append(Long.toUnsignedString((state ^ 0x5DEECE66DL) & ((1L << 48) - 1)));
            out.append('\n');
        }
        System.out.print(out);
    }
}
