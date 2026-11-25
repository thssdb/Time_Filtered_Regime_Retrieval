import java.io.File;
import java.nio.file.*;
import java.util.Optional;
import org.apache.tsfile.file.metadata.IDeviceID;
import org.apache.tsfile.read.TsFileSequenceReader;
public class WriteResource {
  static void check(boolean test, String description) throws Exception {
    if (!test) throw new java.io.IOException(description);
  }
  static void resource(Path file) throws Exception{
    Class<?> cls=Class.forName("org.apache.iotdb.db.storageengine.dataregion.tsfile.TsFileResource");
    Object res=cls.getConstructor(File.class).newInstance(file.toFile());
    check(!Files.exists(Path.of(file+".resource")),"existing resource");
    try(var r=new TsFileSequenceReader(file.toString())){
      var devices=r.getAllDevices();var begin=cls.getMethod("updateStartTime",IDeviceID.class,long.class);var end=cls.getMethod("updateEndTime",IDeviceID.class,long.class);
      for(var id:devices){
        var t=r.readChunkMetadataInDevice(id).get("");check(t.size()==1,"time chunk");
        begin.invoke(res,id,t.get(0).getStartTime());end.invoke(res,id,t.get(0).getEndTime());
      }
      cls.getMethod("serialize").invoke(res);
      Object loaded=cls.getConstructor(File.class).newInstance(file.toFile());cls.getMethod("deserialize").invoke(loaded);
      for(var id:devices){
        var t=r.readChunkMetadataInDevice(id).get("").get(0);
        check(Optional.of(t.getStartTime()).equals(cls.getMethod("getStartTime",IDeviceID.class).invoke(loaded,id)) && Optional.of(t.getEndTime()).equals(cls.getMethod("getEndTime",IDeviceID.class).invoke(loaded,id)),"resource time mismatch");
      }
    }
  }
  public static void main(String[] args) throws Exception { resource(Path.of(args[0])); }
}
