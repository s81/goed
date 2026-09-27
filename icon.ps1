# Draws goed.ico and embeds it into goed.exe as icon group 1.
# TCC can't compile .rc files, so run this after every build:  pwsh -File icon.ps1
param([string]$Exe = "$PSScriptRoot\goed.exe")
Add-Type -AssemblyName System.Drawing

function Draw([int]$s) {
    $bmp = [Drawing.Bitmap]::new($s, $s)
    $g = [Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'
    $g.TextRenderingHint = 'AntiAliasGridFit'

    # Go-blue rounded square
    $d = [single]($s * 0.36); $e = [single]($s - 1)
    $p = [Drawing.Drawing2D.GraphicsPath]::new()
    $p.AddArc(0, 0, $d, $d, 180, 90); $p.AddArc($e - $d, 0, $d, $d, 270, 90)
    $p.AddArc($e - $d, $e - $d, $d, $d, 0, 90); $p.AddArc(0, $e - $d, $d, $d, 90, 90)
    $p.CloseFigure()
    $g.FillPath([Drawing.SolidBrush]::new([Drawing.Color]::FromArgb(0, 173, 216)), $p)

    # "go" (just "g" when tiny) followed by a text cursor
    $txt = if ($s -le 24) { 'g' } else { 'go' }
    $font = [Drawing.Font]::new('Consolas', [single]($s * 0.52), [Drawing.FontStyle]::Bold, [Drawing.GraphicsUnit]::Pixel)
    $fmt = [Drawing.StringFormat]::GenericTypographic
    $sz = $g.MeasureString($txt, $font, 1000, $fmt)
    $barW = [Math]::Max(1, [int]($s * 0.07)); $gap = [Math]::Max(1, [int]($s * 0.04))
    $x = [single](($s - $sz.Width - $gap - $barW) / 2)
    $y = [single](($s - $sz.Height) / 2 - $s * 0.03)
    $white = [Drawing.Brushes]::White
    $g.DrawString($txt, $font, $white, $x, $y, $fmt)
    $g.FillRectangle($white, [single]($x + $sz.Width + $gap), [single]($s * 0.24), [single]$barW, [single]($s * 0.52))

    $g.Dispose()
    $ms = [IO.MemoryStream]::new()
    $bmp.Save($ms, [Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    , $ms.ToArray()
}

$sizes = 16, 24, 32, 48, 64, 256
$pngs = foreach ($s in $sizes) { , (Draw $s) }

# .ico file: ICONDIR + ICONDIRENTRY[] + PNG frames
$ico = [IO.MemoryStream]::new(); $w = [IO.BinaryWriter]::new($ico)
$w.Write([uint16]0); $w.Write([uint16]1); $w.Write([uint16]$sizes.Count)
$off = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $d = [byte]($sizes[$i] % 256)   # 256 is stored as 0
    $w.Write($d); $w.Write($d); $w.Write([byte]0); $w.Write([byte]0)
    $w.Write([uint16]1); $w.Write([uint16]32); $w.Write([uint32]$pngs[$i].Length); $w.Write([uint32]$off)
    $off += $pngs[$i].Length
}
foreach ($png in $pngs) { $w.Write($png) }
[IO.File]::WriteAllBytes("$PSScriptRoot\goed.ico", $ico.ToArray())

# Resource group: GRPICONDIR + GRPICONDIRENTRY[] (entry id instead of file offset)
$grp = [IO.MemoryStream]::new(); $w = [IO.BinaryWriter]::new($grp)
$w.Write([uint16]0); $w.Write([uint16]1); $w.Write([uint16]$sizes.Count)
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $d = [byte]($sizes[$i] % 256)
    $w.Write($d); $w.Write($d); $w.Write([byte]0); $w.Write([byte]0)
    $w.Write([uint16]1); $w.Write([uint16]32); $w.Write([uint32]$pngs[$i].Length); $w.Write([uint16]($i + 1))
}

Add-Type -Namespace Goed -Name Res -MemberDefinition @'
[DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
public static extern IntPtr BeginUpdateResource(string file, bool deleteExisting);
[DllImport("kernel32.dll", SetLastError = true)]
public static extern bool UpdateResource(IntPtr h, IntPtr type, IntPtr name, ushort lang, byte[] data, uint size);
[DllImport("kernel32.dll", SetLastError = true)]
public static extern bool EndUpdateResource(IntPtr h, bool discard);
'@
$h = [Goed.Res]::BeginUpdateResource((Resolve-Path $Exe).Path, $false)
if ($h -eq [IntPtr]::Zero) { throw "BeginUpdateResource failed: $([Runtime.InteropServices.Marshal]::GetLastWin32Error())" }
$ok = $true
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $ok = $ok -and [Goed.Res]::UpdateResource($h, [IntPtr]3, [IntPtr]($i + 1), 0, $pngs[$i], $pngs[$i].Length)   # RT_ICON
}
$g = $grp.ToArray()
$ok = $ok -and [Goed.Res]::UpdateResource($h, [IntPtr]14, [IntPtr]1, 0, $g, $g.Length)                         # RT_GROUP_ICON
if (-not $ok) { [void][Goed.Res]::EndUpdateResource($h, $true); throw "UpdateResource failed" }
if (-not [Goed.Res]::EndUpdateResource($h, $false)) { throw "EndUpdateResource failed" }
"icon embedded into $Exe"
