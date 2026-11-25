import java.io.*;
import java.nio.*;
import java.nio.file.*;
import java.util.*;
import org.apache.tsfile.file.metadata.*;
import org.apache.tsfile.read.*;
import org.apache.tsfile.read.common.Chunk;
import org.apache.tsfile.utils.ReadWriteForEncodingUtils;
import org.apache.tsfile.write.writer.TsFileIOWriter;

public class Embed {
  static int block(IDeviceID id) {
    String s=id.toString();return Integer.parseInt(s.substring(s.lastIndexOf(".b")+2));
  }
  static List<ChunkMetadata> chunks(TsFileSequenceReader r, IDeviceID id) throws Exception {
    List<ChunkMetadata> out=new ArrayList<>();
    r.readChunkMetadataInDevice(id).values().forEach(out::addAll);
    out.sort(Comparator.comparingLong(ChunkMetadata::getOffsetOfChunkHeader));return out;
  }
  public static void main(String[] args) throws Exception {
    Path source=Path.of(args[0]),packs=Path.of(args[1]),dest=Path.of(args[2]);int g=Integer.parseInt(args[3]);
    List<Path> files;
    try(var paths=Files.list(source)){files=paths.filter(p->p.toString().endsWith(".tsfile")).sorted().toList();}
    TreeMap<Integer,Integer> ord=new TreeMap<>();
    for(Path f:files) try(var r=new TsFileSequenceReader(f.toString())){for(var id:r.getAllDevices())ord.put(block(id),0);}
    int sequence=0;for(int b:ord.keySet())ord.put(b,sequence++);
    byte[][][] pages=new byte[g][sequence][];
    for(int c=0;c<g;c++)try(var in=new DataInputStream(Files.newInputStream(packs.resolve(c+".pages")))){
      for(int b=0;b<sequence;b++){int len=in.readInt();pages[c][b]=in.readNBytes(len);if(pages[c][b].length!=len)throw new EOFException();}
      if(in.read()!=-1)throw new IOException("page count");
      Files.copy(packs.resolve(c+".dict"),dest.resolve(c+".dict"),StandardCopyOption.REPLACE_EXISTING);
    }
    Path dat=dest.resolve("data");Files.createDirectories(dat);
    for(Path f:files){
      Path out=dat.resolve(f.getFileName());if(Files.exists(out))throw new IOException("existing TsFile");
      try(var r=new TsFileSequenceReader(f.toString());var writer=new TsFileIOWriter(out.toFile())){
        for(var id:r.getAllDevices()){
          writer.startChunkGroup(id);
          for(var m:chunks(r,id)){
            Chunk ch=r.readMemChunk(m);var head=ch.getHeader();var buf=ch.getData().duplicate();
            String name=head.getMeasurementID();int mask=head.getChunkType()&0xc0;
            if(mask!=0x80&&!name.equals("vector_id")){
              int c=name.startsWith("c")?Integer.parseInt(name.substring(1)):List.of("mp","ip","n2").indexOf(name);
              int n=(int)m.getNumOfPoints();byte[] payload=pages[c][ord.get(block(id))];
              int size=4+(n+7)/8+payload.length;var os=new ByteArrayOutputStream();
              ReadWriteForEncodingUtils.writeUnsignedVarInt(size,os);ReadWriteForEncodingUtils.writeUnsignedVarInt(size,os);
              var ds=new DataOutputStream(os);ds.writeInt(n);
              for(int j=0;j<(n+7)/8;j++)ds.writeByte((j+1)*8<=n?255:(255<<(8-n%8))&255);
              ds.write(payload);buf=ByteBuffer.wrap(os.toByteArray());
            }
            long begin=writer.getPos();
            writer.startFlushChunk(name,head.getCompressionType(),head.getDataType(),head.getEncodingType(),m.getStatistics(),buf.remaining(),1,mask);
            writer.getIOWriterOut().write(buf);writer.endCurrentChunk();
            System.out.println("CHUNK,"+(mask==0x80?"time":name)+","+(writer.getPos()-begin));
          }
          writer.endChunkGroup();
        }
        writer.endFile();
      }
      if (args.length == 4) WriteResource.resource(out);
      else if (args.length != 5 || !args[4].equals("--pages-only")) throw new IllegalArgumentException("unknown resource option");
    }
  }
}
