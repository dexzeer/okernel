# Measures the pitch of what Windows is actually playing: a WASAPI loopback capture of the default output device, then the
# strongest frequency near -Freq in every -Window seconds. Test tool for the sound card speed question (is the sound that
# reaches the speakers at the right speed?): play a very quiet (-60 dB) 1 kHz tone, run this, compare.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\media\loopback_tone.ps1 -Seconds 50 -Freq 1000
param([string]$WavFile = "", [double]$Seconds = 40, [double]$HighPass = 17000, [double]$Freq = 1000, [double]$Window = 2, [double]$MinRms = 0.00005)

Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Threading;

[ComImport, Guid("BCDE0395-E52F-467C-8E3D-C4579291692E")] class MMDeviceEnumeratorCo {}

[ComImport, Guid("A95664D2-9614-4F35-A746-DE8DB63617E6"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IMMDeviceEnumerator {
    int EnumAudioEndpoints(int dataFlow, int stateMask, out IntPtr devices);
    int GetDefaultAudioEndpoint(int dataFlow, int role, out IMMDevice device);
}

[ComImport, Guid("D666063F-1587-4E43-81F1-B948E807363F"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IMMDevice {
    int Activate(ref Guid iid, int clsCtx, IntPtr activationParams, [MarshalAs(UnmanagedType.IUnknown)] out object iface);
}

[ComImport, Guid("1CB9AD4C-DBFA-4c32-B178-C2F568A703B2"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IAudioClient {
    int Initialize(int shareMode, int streamFlags, long bufferDuration, long periodicity, IntPtr format, IntPtr sessionGuid);
    int GetBufferSize(out uint frames);
    int GetStreamLatency(out long latency);
    int GetCurrentPadding(out uint padding);
    int IsFormatSupported(int shareMode, IntPtr format, out IntPtr closest);
    int GetMixFormat(out IntPtr format);
    int GetDevicePeriod(out long defaultPeriod, out long minimumPeriod);
    int Start();
    int Stop();
    int Reset();
    int SetEventHandle(IntPtr h);
    int GetService(ref Guid iid, [MarshalAs(UnmanagedType.IUnknown)] out object service);
}

[ComImport, Guid("C8ADBD64-E71E-48a0-A4DE-185C395CD317"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IAudioCaptureClient {
    int GetBuffer(out IntPtr data, out uint frames, out uint flags, out ulong devicePos, out ulong qpcPos);
    int ReleaseBuffer(uint frames);
    int GetNextPacketSize(out uint frames);
}

public static class Loop {
    public static int Rate; public static int Channels;
    // returns channel-0 samples (float) of the default output device's mix; silence delivers no packets, so gaps are possible
    public static float[] Capture(double seconds) {
        var en = (IMMDeviceEnumerator)new MMDeviceEnumeratorCo();
        IMMDevice dev; Marshal.ThrowExceptionForHR(en.GetDefaultAudioEndpoint(0, 0, out dev));
        Guid iidClient = new Guid("1CB9AD4C-DBFA-4c32-B178-C2F568A703B2");
        object o; Marshal.ThrowExceptionForHR(dev.Activate(ref iidClient, 23, IntPtr.Zero, out o));
        var ac = (IAudioClient)o;
        IntPtr fmt; Marshal.ThrowExceptionForHR(ac.GetMixFormat(out fmt));
        Channels = Marshal.ReadInt16(fmt, 2); Rate = Marshal.ReadInt32(fmt, 4);
        int bits = Marshal.ReadInt16(fmt, 14); int tag = Marshal.ReadInt16(fmt, 0);
        bool isFloat = tag == 3 || (tag == unchecked((short)0xFFFE) && bits == 32);
        Marshal.ThrowExceptionForHR(ac.Initialize(0, 0x00020000, 10000000L, 0, fmt, IntPtr.Zero));
        Guid iidCap = new Guid("C8ADBD64-E71E-48a0-A4DE-185C395CD317");
        object oc; Marshal.ThrowExceptionForHR(ac.GetService(ref iidCap, out oc));
        var cap = (IAudioCaptureClient)oc;
        var outList = new List<float>();
        Marshal.ThrowExceptionForHR(ac.Start());
        var sw = System.Diagnostics.Stopwatch.StartNew();
        while (sw.Elapsed.TotalSeconds < seconds) {
            uint n; cap.GetNextPacketSize(out n);
            while (n > 0) {
                IntPtr p; uint frames, flags; ulong dp, qp;
                Marshal.ThrowExceptionForHR(cap.GetBuffer(out p, out frames, out flags, out dp, out qp));
                bool silent = (flags & 2) != 0;
                for (uint i = 0; i < frames; i++) {
                    float v = 0;
                    if (!silent) {
                        if (isFloat) { int raw = Marshal.ReadInt32(p, (int)(i * Channels * 4)); v = BitConverter.ToSingle(BitConverter.GetBytes(raw), 0); }
                        else if (bits == 16) v = Marshal.ReadInt16(p, (int)(i * Channels * 2)) / 32768f;
                        else v = Marshal.ReadInt32(p, (int)(i * Channels * 4)) / 2147483648f;
                    }
                    outList.Add(v);
                }
                cap.ReleaseBuffer(frames);
                cap.GetNextPacketSize(out n);
            }
            Thread.Sleep(5);
        }
        ac.Stop();
        return outList.ToArray();
    }
    // steep high-pass (cascaded RBJ biquads, Q 0.707) so that the music a user may be playing at the same time does not drown a -40 dB tone
    public static void HighPass(float[] x, double fc, double rate, int stages) {
        double w = 2 * Math.PI * fc / rate, cw = Math.Cos(w), al = Math.Sin(w) / (2 * 0.7071);
        double b0 = (1 + cw) / 2, b1 = -(1 + cw), b2 = (1 + cw) / 2, a0 = 1 + al, a1 = -2 * cw, a2 = 1 - al;
        b0 /= a0; b1 /= a0; b2 /= a0; a1 /= a0; a2 /= a0;
        for (int s = 0; s < stages; s++) {
            double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
            for (int i = 0; i < x.Length; i++) {
                double v = x[i], y = b0 * v + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
                x2 = x1; x1 = v; y2 = y1; y1 = y; x[i] = (float)y;
            }
        }
    }
    // phase discontinuities of a tone at frequency f: demodulate in 2 ms blocks, count block-to-block phase steps over 0.6 rad (a dropped or repeated chunk of audio is a phase step)
    public static int Jumps(float[] x, int from, int len, double f, double rate) {
        int blk = (int)(rate * 0.002), n = len / blk, jumps = 0; double prev = 0, w = 2 * Math.PI * f / rate;
        for (int b = 0; b < n; b++) {
            double re = 0, im = 0;
            for (int i = 0; i < blk; i++) { int k = from + b * blk + i; double a = w * k; re += x[k] * Math.Cos(a); im -= x[k] * Math.Sin(a); }
            double ph = Math.Atan2(im, re);
            if (b > 0) { double d = ph - prev; while (d > Math.PI) d -= 2 * Math.PI; while (d < -Math.PI) d += 2 * Math.PI; if (Math.Abs(d) > 0.6) jumps++; }
            prev = ph;
        }
        return jumps;
    }
    // strongest frequency in [lo, hi] by Goertzel over x[from, from+len)
    public static double Peak(float[] x, int from, int len, double lo, double hi, double rate, out double mag) {
        double best = 0, bestf = 0;
        for (double f = lo; f <= hi; f += 0.5) {
            double w = 2 * Math.PI * f / rate, c = 2 * Math.Cos(w), s1 = 0, s2 = 0;
            for (int i = 0; i < len; i++) { double s = x[from + i] + c * s1 - s2; s2 = s1; s1 = s; }
            double m = s1 * s1 + s2 * s2 - c * s1 * s2;
            if (m > best) { best = m; bestf = f; }
        }
        mag = Math.Sqrt(best) / len * 2;
        return bestf;
    }
}
'@

if ($WavFile) {                                     # analyse a recording (16-bit stereo, 44-byte header) instead of capturing
    $b = [IO.File]::ReadAllBytes($WavFile)
    $n = [int](($b.Length - 44) / 4)
    $x = New-Object float[] $n
    for ($i = 0; $i -lt $n; $i++) { $x[$i] = [BitConverter]::ToInt16($b, 44 + 4 * $i) / 32768.0 }
    [Loop]::Rate = [BitConverter]::ToInt32($b, 24); [Loop]::Channels = 2
} else { $x = [Loop]::Capture($Seconds) }
if ($HighPass -gt 0) { [Loop]::HighPass($x, $HighPass, [Loop]::Rate, 4) }
$rate = [Loop]::Rate
"captured $($x.Length) frames at $rate Hz ($([math]::Round($x.Length / $rate, 1)) s), $([Loop]::Channels) channels"
$w = [int]($Window * $rate)
for ($from = 0; $from + $w -le $x.Length; $from += $w) {
    $sum = 0.0
    for ($i = $from; $i -lt $from + $w; $i++) { $sum += $x[$i] * $x[$i] }
    $rms = [math]::Sqrt($sum / $w)
    if ($rms -lt $MinRms) { "{0,6:N1}s  rms {1:E2}  (silent)" -f ($from / $rate), $rms; continue }
    $mag = 0.0
    $f = [Loop]::Peak($x, $from, $w, $Freq * 0.8, [math]::Min($Freq * 1.22, 23800), $rate, [ref]$mag)
    $m0 = 0.0; $m1 = 0.0; $m2 = 0.0
    [void][Loop]::Peak($x, $from, $w, $Freq, $Freq, $rate, [ref]$m0)
    [void][Loop]::Peak($x, $from, $w, $Freq / 1.19, $Freq / 1.19, $rate, [ref]$m1)
    [void][Loop]::Peak($x, $from, $w, [math]::Min($Freq * 1.19, 23800), [math]::Min($Freq * 1.19, 23800), $rate, [ref]$m2)
    $j = [Loop]::Jumps($x, $from, $w, $f, $rate)
    "{0,6:N1}s  rms {1:E2}  peak {2,7:N1} Hz  (mag {3:E2} purity {7:N2})  phase jumps {8}" -f ($from / $rate), $rms, $f, $mag, $m0, $m1, $m2, ($mag / ($rms * 1.4142)), $j
}
