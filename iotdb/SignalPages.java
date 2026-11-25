import java.io.*;
import java.nio.*;
import java.nio.file.*;
import java.util.*;
import org.apache.tsfile.read.TsFileSequenceReader;
import org.apache.tsfile.utils.ReadWriteForEncodingUtils;

final class SignalPages {
  static List<Path> files(Path dir) throws Exception {
    try (var s = Files.list(dir)) {
      return s.filter(p -> p.toString().endsWith(".tsfile")).sorted().toList();
    }
  }

  static final List<String> names = List.of("mp", "ip", "n2");

  static SeekPack[] read(Path dir) throws Exception {
    var blocks = new TreeMap<Integer, byte[][]>();
    for (Path file : files(dir))
      try (var r = new TsFileSequenceReader(file.toString())) {
        for (var id : r.getAllDevices()) {
          int b = Embed.block(id);
          byte[][] frame = new byte[3][];
          if (blocks.put(b, frame) != null) throw new IOException("duplicate block");
          for (var m : Embed.chunks(r, id)) {
            int c = names.indexOf(m.getMeasurementUid());
            if (c < 0) continue;
            var chunk = r.readMemChunk(m);
            var buf = chunk.getData().duplicate();
            int plain = ReadWriteForEncodingUtils.readUnsignedVarInt(buf);
            int stored = ReadWriteForEncodingUtils.readUnsignedVarInt(buf);
            if (stored != plain || stored != buf.remaining())
              throw new IOException("invalid signal page extent");
            buf = buf.slice();
            int n = buf.getInt();
            if (n != m.getNumOfPoints()) throw new IOException("point count");
            buf.position(4 + (n + 7) / 8);
            frame[c] = new byte[buf.remaining()];
            buf.get(frame[c]);
          }
        }
      }
    var p = new SeekPack[3];
    for (int c = 0; c < 3; c++) {
      var out = new ByteArrayOutputStream();
      for (var block : blocks.values()) out.write(block[c]);
      p[c] = new SeekPack(new ByteArrayInputStream(out.toByteArray()), true, 64, 18);
    }
    return p;
  }
}
