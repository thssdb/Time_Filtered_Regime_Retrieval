import java.nio.*;
import java.nio.channels.*;
import java.nio.file.*;
import java.util.*;
import org.apache.tsfile.common.conf.TSFileConfig;
import org.apache.tsfile.enums.TSDataType;
import org.apache.tsfile.file.metadata.enums.*;
import org.apache.tsfile.write.TsFileWriter;
import org.apache.tsfile.write.record.Tablet;
import org.apache.tsfile.write.schema.*;

public class WriteBlocks {
    static ByteBuffer file(Path p) throws Exception {
        try(FileChannel c=FileChannel.open(p)){
            return c.map(FileChannel.MapMode.READ_ONLY,0,c.size()).order(ByteOrder.LITTLE_ENDIAN);
        }
    }
    static TSFileConfig conf(){
        TSFileConfig c=new TSFileConfig();
        c.setCompressor("UNCOMPRESSED"); c.setFloatPrecision(7); c.setMaxNumberOfPointsInPage(2304);
        c.setPageSizeInByte(64*1024); c.setTimeEncoder("TS_2DIFF");return c;
    }
    // Both sides begin at the earliest vector and use time order to break ties.
    static int[] physicalRows(FloatBuffer raw, IntBuffer order, int begin, int n, boolean similar) {
        int[] rows=new int[n];
        for (int j=0;j<n;j++) rows[j]=order.get(begin+j);
        if (!similar) return rows;
        int[] path=new int[n];
        boolean[] used=new boolean[n];
        int current=0;
        for (int i=0;i<n;i++) {
            path[i]=rows[current]; used[current]=true;
            double best=Double.POSITIVE_INFINITY;
            int next=0;
            for (int j=0;j<n;j++) if (!used[j]) {
                double distance=0;
                for (int dim=0;dim<54;dim++) {
                    double delta=(double)raw.get(rows[current]*54+dim)-raw.get(rows[j]*54+dim);
                    distance+=delta*delta;
                }
                if(distance<best){best=distance;next=j;}
            }
            current=next;
        }
        return path;
    }
    public static void main(String[] args) throws Exception {
        String arm=args[0], ds=args[1], db=args[2];
        Path dir=Path.of(args[3]), src=Path.of(args[4]).resolve(ds);
        List<IMeasurementSchema> cols=new ArrayList<>();
        for(String c:List.of("mp","ip","n2"))cols.add(new MeasurementSchema(c,TSDataType.FLOAT,TSEncoding.PLAIN,CompressionType.UNCOMPRESSED,Map.of("max_point_number","7")));
        cols.add(new MeasurementSchema("vector_id",TSDataType.INT32,TSEncoding.TS_2DIFF,CompressionType.LZ4));
        FloatBuffer raw=file(src.resolve("raw54.f32")).asFloatBuffer();
        LongBuffer times=file(src.resolve("point_time_ns.i64")).asLongBuffer();
        IntBuffer order=file(src.resolve("business_order.u32")).asIntBuffer();
        try(java.io.BufferedWriter placement=Files.newBufferedWriter(dir.resolve(ds+".placement.tsv"));
            TsFileWriter w0=new TsFileWriter(dir.resolve(ds+".0.tsfile").toFile(),new Schema(),conf());
            TsFileWriter w1=new TsFileWriter(dir.resolve(ds+".1.tsfile").toFile(),new Schema(),conf())){
            for(String line:Files.readAllLines(dir.getParent().resolve("blocks.tsv"))){
                String[] f=line.split("\t");
                if(!f[1].equals(ds))continue;
                int id=Integer.parseInt(f[0]), begin=Integer.parseInt(f[3]), n=Integer.parseInt(f[4]);
                TsFileWriter w=(id%2==0)?w0:w1;
                String device=db+"."+f[2];
                w.registerAlignedTimeseries(device,cols);
                Tablet tablet=new Tablet(device,cols,n*18);
                int[] physical=physicalRows(raw,order,begin,n,arm.equals("similarity"));
                placement.write(Integer.toString(id));
                for (int originalId : physical) {placement.write("\t"); placement.write(Integer.toString(originalId));}
                placement.newLine();
                for(int i=0;i<n;i++){
                    int vec=order.get(begin+i);
                    for(int j=0;j<18;j++){
                        int r=i*18+j;
                        tablet.addTimestamp(r,times.get(vec*18+j));
                        for(int s=0;s<3;s++)tablet.addValue(r,s,raw.get(physical[i]*54+s*18+j));
                        if(j==0)tablet.addValue(r,3,vec);
                        else tablet.addValue("vector_id",r,null);
                    }
                }
                w.writeAligned(tablet);w.flush();
            }
        }
    }
}
