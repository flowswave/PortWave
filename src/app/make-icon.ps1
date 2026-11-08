# Generates src/app/PortWave.ico (16-256 px) from the approved PortWave.png artwork.
# Runs on Windows PowerShell 5.1: the drawing and ICO assembly live in C# so byte
# handling stays deterministic. Re-run only when the brand mark changes.
Add-Type -AssemblyName System.Drawing
$ErrorActionPreference = 'Stop'

$source = @'
using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.IO;

public static class PortWaveIcon {
    static GraphicsPath RoundedRect(RectangleF r, float radius) {
        var path = new GraphicsPath();
        float d = radius * 2f;
        path.AddArc(r.X, r.Y, d, d, 180, 90);
        path.AddArc(r.Right - d, r.Y, d, d, 270, 90);
        path.AddArc(r.Right - d, r.Bottom - d, d, d, 0, 90);
        path.AddArc(r.X, r.Bottom - d, d, d, 90, 90);
        path.CloseFigure();
        return path;
    }

    static Bitmap Render(int size, string pngPath) {
        var bmp = new Bitmap(size, size, PixelFormat.Format32bppArgb);
        using (var image = Image.FromFile(pngPath))
        using (var g = Graphics.FromImage(bmp)) {
            g.Clear(Color.Transparent);
            g.CompositingMode = CompositingMode.SourceCopy;
            g.InterpolationMode = InterpolationMode.HighQualityBicubic;
            g.PixelOffsetMode = PixelOffsetMode.HighQuality;
            g.DrawImage(image, new Rectangle(0, 0, size, size));
        }
        return bmp;
    }

    static byte[] Dib(Bitmap bmp) {
        int width = bmp.Width, height = bmp.Height;
        using (var ms = new MemoryStream()) {
            using (var bw = new BinaryWriter(ms, System.Text.Encoding.ASCII, true)) {
                bw.Write(40); bw.Write(width); bw.Write(height * 2);
                bw.Write((short)1); bw.Write((short)32);
                bw.Write(0); bw.Write(width * height * 4);
                bw.Write(0); bw.Write(0); bw.Write(0); bw.Write(0);
                for (int y = height - 1; y >= 0; y--) {
                    for (int x = 0; x < width; x++) {
                        Color c = bmp.GetPixel(x, y);
                        bw.Write(c.B); bw.Write(c.G); bw.Write(c.R); bw.Write(c.A);
                    }
                }
                int maskStride = ((width + 31) / 32) * 4;
                for (int y = 0; y < height; y++)
                    for (int i = 0; i < maskStride; i++) bw.Write((byte)0);
            }
            return ms.ToArray();
        }
    }

    static byte[] Png(Bitmap bmp) {
        using (var ms = new MemoryStream()) {
            bmp.Save(ms, ImageFormat.Png);
            return ms.ToArray();
        }
    }

    public static void Build(string path, string pngPath) {
        int[] sizes = { 16, 20, 24, 32, 48, 64, 128, 256 };
        var payloads = new byte[sizes.Length][];
        for (int i = 0; i < sizes.Length; i++) {
            using (var bmp = Render(sizes[i], pngPath))
                payloads[i] = sizes[i] >= 256 ? Png(bmp) : Dib(bmp);
        }
        using (var stream = File.Create(path))
        using (var writer = new BinaryWriter(stream)) {
            writer.Write((ushort)0); writer.Write((ushort)1); writer.Write((ushort)sizes.Length);
            int offset = 6 + 16 * sizes.Length;
            for (int i = 0; i < sizes.Length; i++) {
                byte dimension = (byte)(sizes[i] >= 256 ? 0 : sizes[i]);
                writer.Write(dimension); writer.Write(dimension);
                writer.Write((byte)0); writer.Write((byte)0);
                writer.Write((ushort)1); writer.Write((ushort)32);
                writer.Write((uint)payloads[i].Length); writer.Write((uint)offset);
                offset += payloads[i].Length;
            }
            foreach (var payload in payloads) writer.Write(payload);
        }
    }
}
'@

Add-Type -TypeDefinition $source -ReferencedAssemblies System.Drawing
$target = Join-Path $PSScriptRoot 'PortWave.ico'
[PortWaveIcon]::Build($target, (Join-Path $PSScriptRoot 'PortWave.png'))
Write-Output ("icon: {0} {1} bytes" -f $target, (Get-Item $target).Length)
