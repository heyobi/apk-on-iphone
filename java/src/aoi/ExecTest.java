package aoi;

/** Runtime.exec, as apps use it (a shell command, getprop, toybox): a child process
 *  runs the guest program with its stdin/stdout as pipes back to this one. */
public final class ExecTest {
    static String run(String... cmd) throws Exception {
        Process p = new ProcessBuilder(cmd).redirectErrorStream(true).start();
        java.io.InputStream in = p.getInputStream();
        java.io.ByteArrayOutputStream b = new java.io.ByteArrayOutputStream();
        byte[] buf = new byte[4096];
        int n;
        while ((n = in.read(buf)) > 0) b.write(buf, 0, n);
        int code = p.waitFor();
        return b.toString("UTF-8").trim() + " (exit " + code + ")";
    }

    public static void main(String[] args) throws Exception {
        if (args.length > 0) { System.out.println("exec: " + run(args)); return; }   /* (one command, for tracing) */
        String a = run("/system/bin/toybox", "echo", "hello");
        String b = run("sh", "-c", "echo $((6*7)); exit 3");
        String c;
        try { run("/system/bin/no-such-program"); c = "ran"; } catch (java.io.IOException e) { c = "IOException"; }
        System.out.println("exec: " + a + ", " + b + ", missing " + c);
    }
}
