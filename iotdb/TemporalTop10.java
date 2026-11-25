package temporal.iotdb;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.nio.ByteBuffer;
import java.nio.channels.Channels;
import java.nio.channels.SocketChannel;
import java.net.StandardProtocolFamily;
import java.net.UnixDomainSocketAddress;
import java.nio.charset.StandardCharsets;
import org.apache.iotdb.udf.api.UDTF;
import org.apache.iotdb.udf.api.access.Row;
import org.apache.iotdb.udf.api.collector.PointCollector;
import org.apache.iotdb.udf.api.customizer.config.UDTFConfigurations;
import org.apache.iotdb.udf.api.customizer.parameter.UDFParameters;
import org.apache.iotdb.udf.api.customizer.strategy.RowByRowAccessStrategy;
import org.apache.iotdb.udf.api.type.Type;

/** One constant-window Top-10 request for an input series containing at least one row. */
public final class TemporalTop10 implements UDTF {
    private String command;
    private String db;
    private boolean sent;
    public void beforeStart(UDFParameters p, UDTFConfigurations c) {
        db=p.getString("dataset");
        String arm=p.getString("arm");
        if (!"ours".equals(arm)) throw new IllegalArgumentException("unknown system");
        if (db == null || !db.matches("[A-Za-z0-9_-]{1,64}"))
            throw new IllegalArgumentException("unknown dataset");
        long lo=Long.parseLong(p.getString("lo")), hi=Long.parseLong(p.getString("hi"));
        String[] coordinates=p.getString("q").split(",",-1);
        if (lo>=hi || coordinates.length!=42)
            throw new IllegalArgumentException("expected lo < hi and 42 coordinates");
        StringBuilder request=new StringBuilder(arm).append(' ').append(lo).append(' ').append(hi);
        for(String coordinate:coordinates) {
            float value=Float.parseFloat(coordinate);
            if(!Float.isFinite(value))throw new IllegalArgumentException("nonfinite query");
            request.append(' ').append(Float.toString(value));
        }
        command=request.append('\n').toString();
        sent=false;
        c.setAccessStrategy(new RowByRowAccessStrategy()).setOutputDataType(Type.TEXT);
    }
    public void transform(Row row, PointCollector out) throws Exception {
        if(sent)return;
        String path=System.getenv("TVM_QUERY_SOCKET_DIR")+"/"+db+".sock";
        try(SocketChannel channel=SocketChannel.open(StandardProtocolFamily.UNIX)) {
            channel.connect(UnixDomainSocketAddress.of(path));
            ByteBuffer input=StandardCharsets.US_ASCII.encode(command);
            while(input.hasRemaining())channel.write(input);
            BufferedReader response=new BufferedReader(new InputStreamReader(Channels.newInputStream(channel),StandardCharsets.US_ASCII));
            String text=response.readLine();
            if(text==null || text.isEmpty() || text.startsWith("ERROR"))
                throw new IllegalStateException("query failed: "+text);
            out.putString(row.getTime(),text);
            sent=true;
        }
    }
}
