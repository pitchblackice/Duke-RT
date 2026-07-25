param(
    [Parameter(Mandatory = $true)] [string]$ReferencePath,
    [Parameter(Mandatory = $true)] [string]$CandidatePath,
    [Parameter(Mandatory = $true)] [string]$SummaryOutput,
    [string]$DifferencePath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.Drawing

if (-not ('NriSrgbPngComparison' -as [type])) {
    Add-Type -ReferencedAssemblies System.Drawing @'
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;
using System.Runtime.InteropServices;

public static class NriSrgbPngComparison
{
    public sealed class Metrics
    {
        public int Width;
        public int Height;
        public long PixelCount;
        public long ChangedPixelCount;
        public double ReferenceMeanLuminance;
        public double CandidateMeanLuminance;
        public double MeanAbsoluteLuminanceError;
        public double MeanLuminanceErrorPercent;
        public double MeanLuminanceBiasPercent;
        public double RootMeanSquareLuminanceError;
        public double P95NormalizedLuminanceErrorPercent;
        public double P99NormalizedLuminanceErrorPercent;
        public double PeakSignalToNoiseRatioDb;
    }

    private static double SrgbToLinear(byte encoded)
    {
        double value = encoded / 255.0;
        return value <= 0.04045 ? value / 12.92 : Math.Pow((value + 0.055) / 1.055, 2.4);
    }

    private static byte[] ReadRgb(string path, out int width, out int height)
    {
        using (Bitmap source = new Bitmap(path))
        using (Bitmap image = new Bitmap(source.Width, source.Height, PixelFormat.Format24bppRgb))
        {
            using (Graphics graphics = Graphics.FromImage(image))
                graphics.DrawImageUnscaled(source, 0, 0);
            width = image.Width;
            height = image.Height;
            Rectangle bounds = new Rectangle(0, 0, width, height);
            BitmapData data = image.LockBits(bounds, ImageLockMode.ReadOnly, PixelFormat.Format24bppRgb);
            try
            {
                int stride = Math.Abs(data.Stride);
                byte[] packed = new byte[width * height * 3];
                byte[] row = new byte[stride];
                for (int y = 0; y < height; ++y)
                {
                    IntPtr rowAddress = data.Stride >= 0
                        ? IntPtr.Add(data.Scan0, y * data.Stride)
                        : IntPtr.Add(data.Scan0, (height - 1 - y) * -data.Stride);
                    Marshal.Copy(rowAddress, row, 0, stride);
                    Buffer.BlockCopy(row, 0, packed, y * width * 3, width * 3);
                }
                return packed;
            }
            finally
            {
                image.UnlockBits(data);
            }
        }
    }

    private static double Percentile(double[] values, double percentile)
    {
        Array.Sort(values);
        int index = Math.Max(0, Math.Min(values.Length - 1,
            (int)Math.Ceiling(percentile * values.Length) - 1));
        return values[index];
    }

    private static void WriteDifference(byte[] reference, byte[] candidate,
        int width, int height, string path)
    {
        string directory = Path.GetDirectoryName(Path.GetFullPath(path));
        if (!String.IsNullOrEmpty(directory))
            Directory.CreateDirectory(directory);
        using (Bitmap image = new Bitmap(width, height, PixelFormat.Format24bppRgb))
        {
            Rectangle bounds = new Rectangle(0, 0, width, height);
            BitmapData data = image.LockBits(bounds, ImageLockMode.WriteOnly, PixelFormat.Format24bppRgb);
            try
            {
                int stride = Math.Abs(data.Stride);
                byte[] row = new byte[stride];
                for (int y = 0; y < height; ++y)
                {
                    Array.Clear(row, 0, row.Length);
                    int sourceRow = y * width * 3;
                    for (int x = 0; x < width * 3; ++x)
                    {
                        int difference = Math.Abs(candidate[sourceRow + x] - reference[sourceRow + x]);
                        row[x] = (byte)Math.Min(255, difference * 4);
                    }
                    IntPtr rowAddress = data.Stride >= 0
                        ? IntPtr.Add(data.Scan0, y * data.Stride)
                        : IntPtr.Add(data.Scan0, (height - 1 - y) * -data.Stride);
                    Marshal.Copy(row, 0, rowAddress, stride);
                }
            }
            finally
            {
                image.UnlockBits(data);
            }
            image.Save(path, ImageFormat.Png);
        }
    }

    public static Metrics Compare(string referencePath, string candidatePath, string differencePath)
    {
        int referenceWidth, referenceHeight, candidateWidth, candidateHeight;
        byte[] reference = ReadRgb(referencePath, out referenceWidth, out referenceHeight);
        byte[] candidate = ReadRgb(candidatePath, out candidateWidth, out candidateHeight);
        if (referenceWidth != candidateWidth || referenceHeight != candidateHeight)
            throw new InvalidDataException("PNG dimensions differ.");

        int pixelCount = referenceWidth * referenceHeight;
        double[] referenceLuminance = new double[pixelCount];
        double[] candidateLuminance = new double[pixelCount];
        double referenceSum = 0.0;
        double candidateSum = 0.0;
        long changedPixels = 0;
        for (int pixel = 0; pixel < pixelCount; ++pixel)
        {
            int offset = pixel * 3;
            double referenceY = 0.0722 * SrgbToLinear(reference[offset])
                + 0.7152 * SrgbToLinear(reference[offset + 1])
                + 0.2126 * SrgbToLinear(reference[offset + 2]);
            double candidateY = 0.0722 * SrgbToLinear(candidate[offset])
                + 0.7152 * SrgbToLinear(candidate[offset + 1])
                + 0.2126 * SrgbToLinear(candidate[offset + 2]);
            referenceLuminance[pixel] = referenceY;
            candidateLuminance[pixel] = candidateY;
            referenceSum += referenceY;
            candidateSum += candidateY;
            if (reference[offset] != candidate[offset]
                || reference[offset + 1] != candidate[offset + 1]
                || reference[offset + 2] != candidate[offset + 2])
                ++changedPixels;
        }

        double referenceMean = referenceSum / pixelCount;
        double candidateMean = candidateSum / pixelCount;
        double normalizationFloor = Math.Max(1.0e-6, referenceMean * 0.01);
        double absoluteSum = 0.0;
        double squaredSum = 0.0;
        double[] normalizedErrors = new double[pixelCount];
        for (int pixel = 0; pixel < pixelCount; ++pixel)
        {
            double error = Math.Abs(candidateLuminance[pixel] - referenceLuminance[pixel]);
            absoluteSum += error;
            squaredSum += error * error;
            normalizedErrors[pixel] = error / Math.Max(referenceLuminance[pixel], normalizationFloor);
        }
        double meanAbsolute = absoluteSum / pixelCount;
        double rootMeanSquare = Math.Sqrt(squaredSum / pixelCount);
        if (!String.IsNullOrWhiteSpace(differencePath))
            WriteDifference(reference, candidate, referenceWidth, referenceHeight, differencePath);

        return new Metrics {
            Width = referenceWidth,
            Height = referenceHeight,
            PixelCount = pixelCount,
            ChangedPixelCount = changedPixels,
            ReferenceMeanLuminance = referenceMean,
            CandidateMeanLuminance = candidateMean,
            MeanAbsoluteLuminanceError = meanAbsolute,
            MeanLuminanceErrorPercent = 100.0 * meanAbsolute / Math.Max(referenceMean, 1.0e-6),
            MeanLuminanceBiasPercent = 100.0 * (candidateMean - referenceMean) / Math.Max(referenceMean, 1.0e-6),
            RootMeanSquareLuminanceError = rootMeanSquare,
            P95NormalizedLuminanceErrorPercent = 100.0 * Percentile(normalizedErrors, 0.95),
            P99NormalizedLuminanceErrorPercent = 100.0 * Percentile(normalizedErrors, 0.99),
            PeakSignalToNoiseRatioDb = rootMeanSquare > 0.0 ? 20.0 * Math.Log10(1.0 / rootMeanSquare) : Double.PositiveInfinity
        };
    }
}
'@
}

$reference = (Resolve-Path -LiteralPath $ReferencePath -ErrorAction Stop).Path
$candidate = (Resolve-Path -LiteralPath $CandidatePath -ErrorAction Stop).Path
$resolvedDifference = if ([string]::IsNullOrWhiteSpace($DifferencePath)) {
    $null
} else {
    [System.IO.Path]::GetFullPath($DifferencePath)
}
$metrics = [NriSrgbPngComparison]::Compare($reference, $candidate, $resolvedDifference)
$summary = [ordered]@{
    schema = 1
    ok = $true
    qualityEvidence = 'display-referred-tonemapped'
    linearHdrEvidence = $false
    colorDomain = 'png_srgb_decoded_to_linear_display_luminance'
    warning = 'These metrics include tone mapping, exposure, bloom, denoising, and 8-bit quantization; they are not a linear scene-HDR oracle.'
    referencePath = $reference
    referenceSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $reference).Hash.ToLowerInvariant()
    candidatePath = $candidate
    candidateSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $candidate).Hash.ToLowerInvariant()
    differencePath = $resolvedDifference
    differenceGain = if ($null -ne $resolvedDifference) { 4 } else { $null }
    width = $metrics.Width
    height = $metrics.Height
    pixelCount = $metrics.PixelCount
    changedPixelCount = $metrics.ChangedPixelCount
    changedPixelPercent = [Math]::Round(100.0 * $metrics.ChangedPixelCount / $metrics.PixelCount, 6)
    referenceMeanLuminance = [Math]::Round($metrics.ReferenceMeanLuminance, 9)
    candidateMeanLuminance = [Math]::Round($metrics.CandidateMeanLuminance, 9)
    meanAbsoluteLuminanceError = [Math]::Round($metrics.MeanAbsoluteLuminanceError, 9)
    meanLuminanceErrorPercent = [Math]::Round($metrics.MeanLuminanceErrorPercent, 6)
    meanLuminanceBiasPercent = [Math]::Round($metrics.MeanLuminanceBiasPercent, 6)
    rootMeanSquareLuminanceError = [Math]::Round($metrics.RootMeanSquareLuminanceError, 9)
    p95NormalizedLuminanceErrorPercent = [Math]::Round($metrics.P95NormalizedLuminanceErrorPercent, 6)
    p99NormalizedLuminanceErrorPercent = [Math]::Round($metrics.P99NormalizedLuminanceErrorPercent, 6)
    peakSignalToNoiseRatioDb = if ([double]::IsPositiveInfinity($metrics.PeakSignalToNoiseRatioDb)) {
        'infinity'
    } else {
        [Math]::Round($metrics.PeakSignalToNoiseRatioDb, 6)
    }
}
$summaryDirectory = Split-Path -Parent $SummaryOutput
if ($summaryDirectory) { New-Item -ItemType Directory -Force -Path $summaryDirectory | Out-Null }
$summary | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $SummaryOutput -Encoding UTF8
$summary | ConvertTo-Json -Depth 5
