import java.io.*;
import java.nio.*;
import java.nio.file.*;
import java.util.*;

/** Build seekable dictionaries from raw little-endian FP32 bits and physical block placement. */
public final class BuildValuePages {
  static List<int[]> blocks(Path placement, int records) throws IOException {
    List<int[]> blocks = new ArrayList<>();
    BitSet seen = new BitSet(records);
    try (var in = Files.newBufferedReader(placement)) {
      for (String line; (line = in.readLine()) != null; ) {
        StringTokenizer fields = new StringTokenizer(line);
        if (Integer.parseInt(fields.nextToken()) != blocks.size())
          throw new IOException("expected sequential block number");
        int[] ids = new int[fields.countTokens()];
        if (ids.length < 1 || ids.length > 128) throw new IOException("invalid block size");
        for (int i = 0; i < ids.length; i++) {
          int id = Integer.parseInt(fields.nextToken());
          if (id < 0 || id >= records || seen.get(id)) throw new IOException("invalid placement");
          seen.set(id);
          ids[i] = id;
        }
        blocks.add(ids);
      }
    }
    if (seen.cardinality() != records) throw new IOException("incomplete placement");
    return blocks;
  }

  // Embed consumes one length-prefixed payload per block. Only block zero includes the dictionary.
  static void split(Path pack, Path pages, List<int[]> blocks, int interval) throws IOException {
    try (var in = new DataInputStream(new BufferedInputStream(Files.newInputStream(pack)));
        var out = new DataOutputStream(new BufferedOutputStream(Files.newOutputStream(pages)))) {
      var bytes = new ByteArrayOutputStream();
      var frame = new DataOutputStream(bytes);
      frame.write(in.readNBytes(6)); // SKR1, reference, interval
      int count = in.readInt();
      frame.writeInt(count);
      frame.write(in.readNBytes(Math.multiplyExact(4, count)));
      int groups = in.readInt();
      frame.writeInt(groups);
      for (int[] ids : blocks) {
        for (int row = 0; row < ids.length; row += interval) {
          int length = in.readInt();
          byte[] payload = in.readNBytes(length);
          if (payload.length != length
              || Byte.toUnsignedInt(payload[0]) != Math.min(interval, ids.length - row))
            throw new IOException("invalid group extent");
          frame.writeInt(length);
          frame.write(payload);
          groups--;
        }
        out.writeInt(bytes.size());
        bytes.writeTo(out);
        bytes.reset();
      }
      if (groups != 0 || in.read() != -1) throw new IOException("unexpected number of groups");
    }
  }

  public static void main(String[] args) throws Exception {
    if (args.length != 7 && args.length != 8)
      throw new IllegalArgumentException(
          "raw.f32 placement.tsv destination signals samples group-size first|previous"
              + " [--unsigned-keys]");
    Path dest = Path.of(args[2]);
    int signals = Integer.parseInt(args[3]), samples = Integer.parseInt(args[4]);
    int interval = Integer.parseInt(args[5]);
    boolean first =
        switch (args[6]) {
          case "first" -> true;
          case "previous" -> false;
          default -> throw new IllegalArgumentException("unknown reference");
        };
    if (args.length == 8 && !args[7].equals("--unsigned-keys"))
      throw new IllegalArgumentException("unknown dictionary order");
    int sign = args.length == 8 ? Integer.MIN_VALUE : 0;
    if (signals < 1 || samples < 1 || interval < 2 || interval > 128)
      throw new IllegalArgumentException("invalid dimensions or group size");
    IntBuffer raw = WriteBlocks.file(Path.of(args[0])).asIntBuffer();
    int stride = Math.multiplyExact(samples, signals), n = raw.remaining() / stride;
    if (n < 1 || n * stride != raw.remaining()) throw new IOException("raw dimensions");
    List<int[]> blocks = blocks(Path.of(args[1]), n);
    Files.createDirectories(dest);
    for (int c = 0; c < signals; c++) {
      // The 60-position public dataset writer orders the words as uint32.
      int[] words = new int[n * samples];
      for (int r = 0; r < n; r++)
        for (int j = 0; j < samples; j++)
          words[r * samples + j] = raw.get(r * stride + c * samples + j) ^ sign;
      int[] ordered = Arrays.stream(words).sorted().distinct().toArray();
      int[] dict = Arrays.stream(ordered).map(value -> value ^ sign).toArray();
      List<SeekPack.Page> groups = new ArrayList<>();
      for (int[] ids : blocks) {
        for (int start = 0; start < ids.length; start += interval) {
          int len = Math.min(interval, ids.length - start);
          int[][] ranks = new int[samples][len];
          for (int r = 0; r < len; r++)
            for (int j = 0; j < samples; j++)
              ranks[j][r] = Arrays.binarySearch(ordered, words[ids[start + r] * samples + j]);
          groups.add(new SeekPack.Page(ranks));
        }
      }
      Path pack = dest.resolve(c + ".pack");
      SeekPack.write(pack, first, interval, dict, groups);
      new SeekPack(pack, first, interval, samples).verify(groups);
      split(pack, dest.resolve(c + ".pages"), blocks, interval);
    }
  }
}
