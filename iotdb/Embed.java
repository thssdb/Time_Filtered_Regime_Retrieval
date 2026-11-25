import java.io.*;
import java.nio.*;
import java.nio.file.*;
import java.util.*;
import org.apache.tsfile.file.metadata.*;
import org.apache.tsfile.file.metadata.enums.CompressionType;
import org.apache.tsfile.file.metadata.enums.TSEncoding;
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
      Path legacyDictionary=packs.resolve(c+".dict");
      if(Files.exists(legacyDictionary))
        Files.copy(legacyDictionary,dest.resolve(c+".dict"),StandardCopyOption.REPLACE_EXISTING);
    }
    byte[] templateHeader=null;
    byte[][] timePages=null;
    if(Files.exists(packs.resolve("time.header")) || Files.exists(packs.resolve("time.pages"))){
      templateHeader=Files.readAllBytes(packs.resolve("time.header"));
      timePages=new byte[sequence][];
      try(var in=new DataInputStream(Files.newInputStream(packs.resolve("time.pages")))){
        for(int b=0;b<sequence;b++){
          int length=in.readInt();
          if(length<1 || length>(1<<20))throw new IOException("timestamp page size");
          timePages[b]=in.readNBytes(length);
          if(timePages[b].length!=length)throw new EOFException();
        }
        if(in.read()!=-1)throw new IOException("extra timestamp pages");
      }
    }
    Path dat=dest.resolve("data");Files.createDirectories(dat);
    for(Path f:files){
      Path out=dat.resolve(f.getFileName());if(Files.exists(out))throw new IOException("existing TsFile");
      // The native timestamp reader owns the private TIM1 encoding tag (0xfe).
      List<Long> templateTags=new ArrayList<>();
      try(var r=new TsFileSequenceReader(f.toString());var writer=new TsFileIOWriter(out.toFile())){
        for(var id:r.getAllDevices()){
          writer.startChunkGroup(id);
          for(var m:chunks(r,id)){
            Chunk ch=r.readMemChunk(m);var head=ch.getHeader();var buf=ch.getData().duplicate();
            String name=head.getMeasurementID();int mask=head.getChunkType()&0xc0;
            boolean templateTime=mask==0x80 && timePages!=null;
            if(templateTime){
              byte[] page=timePages[ord.get(block(id))];
              if((page[0]&255)*18!=m.getNumOfPoints())throw new IOException("template row count");
              boolean first=templateTags.isEmpty();
              int size=page.length+(first?templateHeader.length:0);
              var os=new ByteArrayOutputStream();
              ReadWriteForEncodingUtils.writeUnsignedVarInt(size,os);
              ReadWriteForEncodingUtils.writeUnsignedVarInt(size,os);
              if(first)os.write(templateHeader);
              os.write(page);
              buf=ByteBuffer.wrap(os.toByteArray());
            }else if(mask!=0x80&&!name.equals("vector_id")){
              int c=name.startsWith("c")?Integer.parseInt(name.substring(1)):List.of("mp","ip","n2").indexOf(name);
              int n=(int)m.getNumOfPoints();byte[] payload=pages[c][ord.get(block(id))];
              int size=4+(n+7)/8+payload.length;var os=new ByteArrayOutputStream();
              ReadWriteForEncodingUtils.writeUnsignedVarInt(size,os);ReadWriteForEncodingUtils.writeUnsignedVarInt(size,os);
              var ds=new DataOutputStream(os);ds.writeInt(n);
              for(int j=0;j<(n+7)/8;j++)ds.writeByte((j+1)*8<=n?255:(255<<(8-n%8))&255);
              ds.write(payload);buf=ByteBuffer.wrap(os.toByteArray());
            }
            long begin=writer.getPos();
            writer.startFlushChunk(name,templateTime?CompressionType.UNCOMPRESSED:head.getCompressionType(),
                head.getDataType(),templateTime?TSEncoding.TS_2DIFF:head.getEncodingType(),
                m.getStatistics(),buf.remaining(),1,mask);
            if(templateTime)templateTags.add(writer.getPos()-1);
            writer.getIOWriterOut().write(buf);writer.endCurrentChunk();
            System.out.println("CHUNK,"+(mask==0x80?"time":name)+","+(writer.getPos()-begin));
          }
          writer.endChunkGroup();
        }
        writer.endFile();
      }
      if(!templateTags.isEmpty())try(var raf=new RandomAccessFile(out.toFile(),"rw")){
        for(long position:templateTags){
          raf.seek(position);
          if(raf.readUnsignedByte()!=4)throw new IOException("expected original TS2DIFF encoding");
          raf.seek(position);raf.writeByte(0xfe);
        }
      }
      if (args.length == 4) WriteResource.resource(out);
      else if (args.length != 5 || !args[4].equals("--pages-only")) throw new IllegalArgumentException("unknown resource option");
    }
  }
}
