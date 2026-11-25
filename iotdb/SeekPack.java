import java.io.*;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.*;

// One file per signal. A group contains absolute keys, one Rice parameter per sample position,
// 16-bit byte offsets for rows 2 onward (row 1 starts at zero), then row-major codes.
final class SeekPack {
  final int samples;
  static final int MAGIC = 0x534b5231; // SKR1
  final boolean first;
  final int[] dict;
  final Group[] groups;

  static int zigzag(int value) {
    return (value << 1) ^ (value >> 31);
  }

  static int unzigzag(int value) {
    return (value >>> 1) ^ -(value & 1);
  }

  static final class Bits {
    final ByteArrayOutputStream output = new ByteArrayOutputStream();
    int current;
    int used;

    void bit(int value) {
      current |= (value & 1) << used++;
      if (used == 8) {
        output.write(current);
        current = 0;
        used = 0;
      }
    }

    void fixed(int value, int width) {
      for (int i = 0; i < width; ++i) bit(value >>> i);
    }

    byte[] finish() {
      if (used != 0) output.write(current);
      return output.toByteArray();
    }
  }

  static final class BitInput {
    final byte[] source;
    int bit;

    BitInput(byte[] source, int bit) {
      this.source = source;
      this.bit = bit;
    }

    int one() throws IOException {
      if (bit >= 8 * source.length) throw new EOFException("Rice stream ended early");
      return (source[bit >>> 3] >>> (bit++ & 7)) & 1;
    }

    int fixed(int width) throws IOException {
      int value = 0;
      for (int i = 0; i < width; ++i) value |= one() << i;
      return value;
    }
  }

  static int riceParameter(int[] transformed) {
    int bestK = 0;
    long bestBits = Long.MAX_VALUE;
    for (int k = 0; k <= 15; ++k) {
      long bits = 0;
      for (int row = 1; row < transformed.length; ++row) {
        int value = zigzag(transformed[row]);
        bits += (value >>> k) + 1L + k;
      }
      if (bits < bestBits) {
        bestBits = bits;
        bestK = k;
      }
    }
    return bestK;
  }

  record Page(int[][] ranks) {
    int rows() {
      return ranks[0].length;
    }
  }

  record Target(int group, int row, int position) {}

  static final class Group {
    final int rows;
    final byte[] k;
    final short[] offsets;
    final byte[] body;
    final int[] anchor; // Absolute keys unpacked once when opening the resident reader.

    Group(DataInputStream group, int rankWidth, int maxRows, int J) throws IOException {
      rows = group.readUnsignedByte();
      if (rows == 0 || rows > maxRows) throw new IOException("Invalid group length");
      byte[] abs = group.readNBytes((J * rankWidth + 7) / 8);
      var bits = new BitInput(abs, 0);
      anchor = new int[J];
      for (int j = 0; j < J; j++) anchor[j] = bits.fixed(rankWidth);
      k = group.readNBytes(rows > 1 ? J : 0);
      offsets = new short[Math.max(0, rows - 2)];
      for (int r = 0; r < offsets.length; r++) offsets[r] = group.readShort();
      body = group.readAllBytes();
      if (offsets.length > 0 && Short.toUnsignedInt(offsets[offsets.length - 1]) >= body.length)
        throw new IOException("Invalid segment offset");
    }

    int offset(int row) {
      return row == 1 ? 0 : Short.toUnsignedInt(offsets[row - 2]);
    }
  }

  SeekPack(Path file, boolean expectedFirst, int interval, int samples) throws IOException {
    this(Files.newInputStream(file), expectedFirst, interval, samples);
  }

  SeekPack(InputStream stream, boolean expectedFirst, int interval, int samples)
      throws IOException {
    this.samples = samples;
    try (var in = new DataInputStream(new BufferedInputStream(stream))) {
      if (in.readInt() != MAGIC) throw new IOException("Invalid pack format");
      first = in.readBoolean();
      if (first != expectedFirst || in.readUnsignedByte() != interval)
        throw new IOException("Wrong arm");
      dict = new int[in.readInt()];
      for (int i = 0; i < dict.length; i++) dict[i] = in.readInt();
      int width = 32 - Integer.numberOfLeadingZeros(dict.length - 1);
      groups = new Group[in.readInt()];
      for (int i = 0; i < groups.length; i++) {
        int length = in.readInt();
        byte[] frame = in.readNBytes(length);
        if (frame.length != length) throw new EOFException();
        groups[i] =
            new Group(
                new DataInputStream(new ByteArrayInputStream(frame)), width, interval, samples);
      }
      if (in.read() != -1) throw new IOException("Trailing bytes");
    }
  }

  static void write(Path path, boolean first, int interval, int[] dict, List<Page> pages)
      throws IOException {
    Files.createDirectories(path.getParent());
    int width = 32 - Integer.numberOfLeadingZeros(dict.length - 1);
    final int J = pages.get(0).ranks.length;
    try (var out = new DataOutputStream(new BufferedOutputStream(Files.newOutputStream(path)))) {
      out.writeInt(MAGIC);
      out.writeBoolean(first);
      out.writeByte(interval);
      out.writeInt(dict.length);
      for (int value : dict) out.writeInt(value);
      out.writeInt(pages.size());
      for (var page : pages) {
        int n = page.rows();
        var abs = new Bits();
        int[] k = new int[J];
        for (int j = 0; j < J; j++) {
          int[] lane = page.ranks[j];
          abs.fixed(lane[0], width);
          if (n > 1) {
            int[] codes = new int[n];
            codes[0] = lane[0];
            for (int r = 1; r < n; r++) codes[r] = lane[r] - lane[first ? 0 : r - 1];
            // Minimize the Rice payload at this sample position.
            k[j] = riceParameter(codes);
          }
        }
        var payload = new ByteArrayOutputStream();
        var header = new ByteArrayOutputStream();
        var frame = new DataOutputStream(header);
        frame.writeByte(n);
        frame.write(abs.finish());
        if (n > 1) for (int param : k) frame.writeByte(param);
        for (int r = 1; r < n; r++) {
          if (r >= 2) {
            if (payload.size() > 65535) throw new IOException("Group offset exceeds 16 bits");
            frame.writeShort(payload.size());
          }
          var bits = new Bits();
          for (int j = 0; j < J; j++) {
            int u = zigzag(page.ranks[j][r] - page.ranks[j][first ? 0 : r - 1]);
            for (int q = u >>> k[j]; q > 0; q--) bits.bit(0);
            bits.bit(1);
            bits.fixed(u, k[j]);
          }
          payload.write(bits.finish());
        }
        out.writeInt(header.size() + payload.size());
        header.writeTo(out);
        payload.writeTo(out);
      }
    }
  }

  // Called by both arms. One seek reads the requested segment’s Rice codewords.
  void decodeRow(Group g, int row, int[] reference, int[] keys) throws IOException {
    final int J = samples;
    var bits = new BitInput(g.body, g.offset(row) * 8);
    for (int j = 0; j < J; j++) {
      int q = 0;
      while (bits.one() == 0) q++;
      int u = (q << g.k[j]) | bits.fixed(g.k[j]);
      keys[j] = reference[j] + unzigzag(u);
    }
  }

  // Targets arrive sorted by increasing position. Return the number of non-anchor
  // segment payloads actually visited, also checked by the cold verification pass.
  int decodeTargets(int group, Target[] targets, int[][] out, int col) throws IOException {
    final int J = samples;
    Group g = groups[group];
    int[] keys = g.anchor.clone();
    int visited = 0, previousRow = 0;
    for (var target : targets) {
      int row = target.row();
      if (row < previousRow || row >= g.rows)
        throw new IOException("Invalid target order or position");
      if (row > previousRow) {
        if (first) {
          decodeRow(g, row, g.anchor, keys);
          visited++;
        } else {
          for (int r = previousRow + 1; r <= row; r++) {
            decodeRow(g, r, keys, keys);
            visited++;
          }
        }
      }
      for (int j = 0; j < J; j++) out[target.position()][col * J + j] = dict[keys[j]];
      previousRow = row;
    }
    return visited;
  }

  void verify(List<Page> source) throws IOException {
    final int J = samples;
    if (groups.length != source.size()) throw new IOException("Page count mismatch");
    for (int i = 0; i < source.size(); i++) {
      var page = source.get(i);
      if (page.rows() != groups[i].rows) throw new IOException("Group length mismatch");
      var targets = new Target[page.rows()];
      int[][] out = new int[page.rows()][J];
      for (int r = 0; r < targets.length; r++) targets[r] = new Target(i, r, r);
      decodeTargets(i, targets, out, 0);
      for (int r = 0; r < page.rows(); r++)
        for (int j = 0; j < J; j++)
          if (out[r][j] != dict[page.ranks[j][r]]) throw new IOException("Full readback mismatch");
      int last = targets.length - 1;
      int visits = decodeTargets(i, new Target[] {targets[last]}, out, 0);
      if (visits != (first ? Math.min(last, 1) : last))
        throw new IOException("Unexpected sparse seek count");
      for (int j = 0; j < J; j++)
        if (out[last][j] != dict[page.ranks[j][last]]) throw new IOException("Seek mismatch");
    }
  }
}
