import java.io.*;
import java.nio.*;
import java.nio.file.*;
import java.util.*;

// Resident reader for the SQL bridge. Retrieval ranks index the chronological locator.
public final class GroupRice {
  static final int interval = 64;

  static native void openTime(String directory);

  static native void readTime(int[] locations, long[] timestamps);

  static void require(boolean ok, String message) throws IOException {
    if (!ok) throw new IOException(message);
  }

  static int[] locations(Path path) throws Exception {
    ByteBuffer in = ByteBuffer.wrap(Files.readAllBytes(path)).order(ByteOrder.LITTLE_ENDIAN);
    byte[] magic = new byte[8];
    in.get(magic);
    require(
        Arrays.equals(magic, new byte[] {'N', 'H', 'A', 'D', 'D', 'R', '1', 0}), "address magic");
    require(in.getInt() == 1, "address version");
    int records = in.getInt();
    in.getInt();
    require(in.getInt() == 0 && in.remaining() == 4 * records, "address extent");
    int[] output = new int[records];
    for (int i = 0; i < records; ++i) output[i] = in.getInt();
    return output;
  }

  static void serve(Path data, Path locator, Path library) throws Exception {
    System.load(library.toAbsolutePath().toString());
    var packs = SignalPages.read(data);
    int[] loc = locations(locator);
    openTime(data.toString());
    int blocks = 1 + Arrays.stream(loc).map(v -> v >>> 14).max().orElseThrow();
    int[] rows = new int[blocks], base = new int[blocks];
    for (int v : loc) rows[v >>> 14]++;
    int groupCount = 0;
    for (int p = 0; p < blocks; p++) {
      base[p] = groupCount;
      groupCount += (rows[p] + interval - 1) / interval;
    }
    for (SeekPack signal : packs) require(groupCount == signal.groups.length, "group count");
    var input = new BufferedReader(new InputStreamReader(System.in));
    var output = new BufferedOutputStream(System.out);
    System.out.println("READY");
    System.out.flush();

    for (String line; (line = input.readLine()) != null; ) {
      StringTokenizer tokens = new StringTokenizer(line);
      int count = Integer.parseInt(tokens.nextToken());
      if (count < 0 || count > 10) throw new IOException("invalid result count");
      int[] ranks = new int[count];
      for (int i = 0; i < count; i++) ranks[i] = Integer.parseInt(tokens.nextToken());
      long begin = System.nanoTime();
      int[] locations = new int[count];
      long[] timestamps = new long[18 * count];
      Map<Integer, List<SeekPack.Target>> targets = new LinkedHashMap<>();
      for (int i = 0; i < count; i++) {
        int v = locations[i] = loc[ranks[i]], row = v & 127, page = base[v >>> 14] + row / interval;
        targets
            .computeIfAbsent(page, key -> new ArrayList<>())
            .add(new SeekPack.Target(page, row % interval, i));
      }
      readTime(locations, timestamps);
      int[][] values = new int[count][54];
      for (var item : targets.entrySet()) {
        var hits = item.getValue().toArray(SeekPack.Target[]::new);
        Arrays.sort(hits, Comparator.comparingInt(SeekPack.Target::row));
        for (int c = 0; c < 3; c++) packs[c].decodeTargets(item.getKey(), hits, values, c);
      }
      long elapsed = System.nanoTime() - begin;
      ByteBuffer payload =
          ByteBuffer.allocate(8 + count * (18 * 8 + 54 * 4)).order(ByteOrder.LITTLE_ENDIAN);
      payload.putLong(elapsed);
      for (int i = 0; i < count; i++) {
        for (int j = 0; j < 18; j++) payload.putLong(timestamps[18 * i + j]);
        for (int value : values[i]) payload.putInt(value);
      }
      output.write(payload.array());
      output.flush();
    }
  }

  public static void main(String[] args) throws Exception {
    serve(Path.of(args[0]), Path.of(args[1]), Path.of(args[2]));
  }
}
